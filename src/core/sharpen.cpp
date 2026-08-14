// SPDX-License-Identifier: MIT
//
// Contrast-adaptive sharpening. See sharpen.h for why it belongs after the
// antialiasing rather than before it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "sharpen.h"
#include "config.h"
#include "d3d11_procs.h"
#include "log.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

template <typename T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

using PFN_D3DCompile = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR,
  const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
  ID3DBlob**, ID3DBlob**);

atfix::mutex g_mutex;
HMODULE g_compiler = nullptr;

// RE-ENTRY GUARD, and it is the general form of a bug that has now bitten
// twice. This pass binds a render target and issues a draw; both go back
// through the hooks that decide when to run it, so it can be asked to run
// itself from inside itself -- and then it blocks on the lock it is holding.
//
// A mutex alone cannot express "not from this call stack", which is the actual
// rule. Thread-local, because a second thread arriving here has every right to
// be served: this engine records on several deferred contexts.
thread_local bool t_inSharpen = false;
bool g_ready = false;
bool g_broken = false;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11Buffer* g_cb = nullptr;

// The scratch the target is copied into, so the shader reads one resource and
// writes another. Kept for the session and resized when the target changes.
ID3D11Texture2D* g_scratch = nullptr;
ID3D11ShaderResourceView* g_scratchSRV = nullptr;
UINT g_scratchWidth = 0, g_scratchHeight = 0;
DXGI_FORMAT g_scratchFormat = DXGI_FORMAT_UNKNOWN;

struct Params { float peak; float pad[3]; };

ID3D11Device* g_ownerDevice = nullptr;

// One device owns everything this pass creates. Handing a second device the
// first one's shaders, states or scratch texture is undefined, so the first
// device to arrive claims the pass and any other is refused. Ported from the
// Dusk project, which carries the same guard in all three of its passes.
//
// The AddRef is what makes the pointer comparison sound. A released device can
// be replaced at the same address by a new one, and the comparison would then
// accept a stranger. Holding a reference keeps the address meaningful for as
// long as the pass can be called, which is the life of the process.
bool acceptsDevice(ID3D11Device* device) {
  if (!g_ownerDevice) {
    g_ownerDevice = device;
    g_ownerDevice->AddRef();
    return true;
  }
  if (g_ownerDevice == device)
    return true;
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true, std::memory_order_relaxed))
    log("SHARPEN: a second D3D11 device reached the shared pass; refusing it"
        " so resources from the first device are never bound across devices");
  return false;
}

// HOW HARD 100% IS ALLOWED TO BE. The shader multiplies its headroom weight by
// this, so it is the strongest the filter can ever pull a neighbour.
//
// AMD's own CAS derives the same number as -1/lerp(8, 5, sharpness), which puts
// their range at 0.125 to 0.2 -- their MINIMUM is already a real sharpen and
// their maximum is the point they stop at. The ceiling here sits between the
// two, at roughly their halfway sharpness. The reason not to go to their top:
// on Ayesha this pass runs at the supersampled size and the downscale that
// follows folds in a sharpen of its own, so the slider at 100% is not the only
// sharpening in the frame.
//
// The floor is well under AMD's, because a slider that starts at 1% should
// start at barely anything rather than at their minimum.
constexpr float kPeakFloor = 0.02f;
constexpr float kPeakCeiling = 0.15f;

// AMD FidelityFX CAS, the sharpening half. The kernel is the four cardinal
// neighbours and the centre: CAS deliberately does not use the diagonals, which
// is what keeps it from ringing on thin features.
//
// The weight comes from the local minimum and maximum. Where they are far apart
// -- a hard edge -- the sharpening is REDUCED, and where they are close the
// filter has room to work. That inversion is the whole idea and it is why this
// can be left on over an interface without haloing it.
const char* kHlsl = R"HLSL(
Texture2D    src  : register(t0);
SamplerState samp : register(s0);
cbuffer Params : register(b0) { float peak; float3 pad; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
  VSOut o;
  float2 t = float2((id << 1) & 2, id & 2);
  o.uv = t;
  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
float3 tap(float2 uv, float2 o, float2 texel) {
  return src.SampleLevel(samp, uv + o * texel, 0).rgb;
}
float4 PSMain(VSOut i) : SV_TARGET {
  float2 size;
  src.GetDimensions(size.x, size.y);
  float2 texel = 1.0 / size;
  float3 c = tap(i.uv, float2( 0,  0), texel);
  float3 n = tap(i.uv, float2( 0, -1), texel);
  float3 s = tap(i.uv, float2( 0,  1), texel);
  float3 w = tap(i.uv, float2(-1,  0), texel);
  float3 e = tap(i.uv, float2( 1,  0), texel);

  float3 mn = min(min(min(n, s), min(w, e)), c);
  float3 mx = max(max(max(n, s), max(w, e)), c);

  // Headroom: how much darker/brighter this neighbourhood can go before it
  // clips. Sharpening is scaled by whichever side has less room.
  float3 room = min(mn, 1.0 - mx);
  float3 weight = sqrt(max(room / max(mx, 1e-4), 0.0));
  weight = -weight * peak;

  float3 sum = (n + s + w + e) * weight + c;
  float3 norm = 1.0 + 4.0 * weight;
  float3 outc = saturate(sum / max(norm, 1e-4));
  return float4(outc, 1.0);
}
)HLSL";

bool compile(PFN_D3DCompile fn, const char* entry, const char* target,
             ID3DBlob** out) {
  ID3DBlob* err = nullptr;
  const HRESULT hr = fn(kHlsl, std::strlen(kHlsl), "dusk_sharpen", nullptr,
                        nullptr, entry, target, 0, 0, out, &err);
  if (FAILED(hr) || !*out)
    log("SHARPEN: ", entry, " failed to compile: ",
        err ? static_cast<const char*>(err->GetBufferPointer()) : "(no message)");
  if (err) err->Release();
  return SUCCEEDED(hr) && *out;
}

bool init(ID3D11Device* device) {
  if (g_ready) return true;

  // THE COMPILER IS LOADED ELSEWHERE, deliberately. LoadLibrary inside a draw
  // detour takes the loader lock on whatever thread the engine is recording on,
  // and this engine records on several -- with edge smoothing on it never
  // showed, because smaa.cpp had already loaded d3dcompiler from its own path
  // and the call returned from cache. With edge smoothing off this was the
  // first caller and Escha hung on the loading screen at 2.4 seconds.
  //
  // sharpenPreload() does it from the frame tick instead, where nothing is
  // holding a graphics lock.
  //
  // Tested BEFORE the one-attempt latch below, because "not preloaded yet" is
  // true at most once and false forever after. sharpenPreload runs from
  // Present, and every caller of sharpenApply runs earlier in the same frame,
  // so the first pass a session asks for can arrive before the first Present
  // has happened. Latching there turned a one-frame wait into sharpening being
  // off for the rest of the process, with a log line that blamed the preload
  // rather than the order.
  HMODULE compiler = g_compiler;
  if (!compiler) {
    static std::atomic<bool> waited { false };
    if (!waited.exchange(true, std::memory_order_relaxed))
      log("SHARPEN: d3dcompiler is not loaded yet; the pass starts on a later"
          " frame");
    return false;
  }
  if (g_broken) return false;
  g_broken = true;   // one attempt
  auto D3DCompile = reinterpret_cast<PFN_D3DCompile>(
    GetProcAddress(compiler, "D3DCompile"));
  if (!D3DCompile) { log("SHARPEN: no D3DCompile"); return false; }

  ID3DBlob* vs = nullptr;
  ID3DBlob* ps = nullptr;
  bool ok = compile(D3DCompile, "VSMain", "vs_4_0", &vs) &&
            compile(D3DCompile, "PSMain", "ps_4_0", &ps);
  if (ok)
    ok = SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(),
           vs->GetBufferSize(), nullptr, &g_vs)) &&
         SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(),
           ps->GetBufferSize(), nullptr, &g_ps));
  release(vs);
  release(ps);
  if (!ok) return false;

  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;   // exact texels; CAS is a kernel
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(device->CreateSamplerState(&sd, &g_sampler))) return false;

  D3D11_BUFFER_DESC cb = {};
  cb.ByteWidth = sizeof(Params);
  cb.Usage = D3D11_USAGE_DYNAMIC;
  cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device->CreateBuffer(&cb, nullptr, &g_cb))) return false;

  // Sharpen colour only. The shader returns alpha 1.0, so an ALL write mask
  // replaces that channel across the whole target. In the Dusk project that
  // produced large intermittent black polygons in later composition on
  // Shallie. An RGB mask preserves alpha whatever the shader returns, which is
  // the contract of a sharpening filter.
  D3D11_BLEND_DESC bd = {};
  bd.RenderTarget[0].RenderTargetWriteMask =
    D3D11_COLOR_WRITE_ENABLE_RED |
    D3D11_COLOR_WRITE_ENABLE_GREEN |
    D3D11_COLOR_WRITE_ENABLE_BLUE;
  if (FAILED(device->CreateBlendState(&bd, &g_blend))) return false;

  D3D11_DEPTH_STENCIL_DESC dd = {};
  dd.DepthEnable = FALSE;
  dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
  if (FAILED(device->CreateDepthStencilState(&dd, &g_depth))) return false;

  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  if (FAILED(device->CreateRasterizerState(&rd, &g_raster))) return false;

  g_broken = false;
  g_ready = true;
  return true;
}

DXGI_FORMAT concrete(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
  }
}

bool ensureScratch(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& sd) {
  const DXGI_FORMAT view = concrete(sd.Format);
  if (g_scratch && g_scratchWidth == sd.Width && g_scratchHeight == sd.Height &&
      g_scratchFormat == view)
    return true;
  release(g_scratchSRV);
  release(g_scratch);
  g_scratchWidth = g_scratchHeight = 0;
  g_scratchFormat = DXGI_FORMAT_UNKNOWN;

  D3D11_TEXTURE2D_DESC td = sd;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.SampleDesc.Count = 1;
  td.SampleDesc.Quality = 0;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  td.CPUAccessFlags = 0;
  td.MiscFlags = 0;
  D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
  vd.Format = view;
  vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  vd.Texture2D.MipLevels = 1;
  // Through the ORIGINAL device: the mod's own surfaces must not travel through
  // the mod's own hooks.
  if (FAILED(getDeviceProcs(device)->CreateTexture2D(device, &td, nullptr,
                                                     &g_scratch)) ||
      !g_scratch ||
      FAILED(device->CreateShaderResourceView(g_scratch, &vd, &g_scratchSRV))) {
    release(g_scratchSRV);
    release(g_scratch);
    return false;
  }
  g_scratchWidth = sd.Width;
  g_scratchHeight = sd.Height;
  g_scratchFormat = view;
  return true;
}

// Everything the pass binds, put back afterwards. The context belongs to the
// game and this runs in the middle of its frame.
struct StateGuard {
  ID3D11DeviceContext* c;
  ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
  UINT vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11InputLayout* il = nullptr;
  ID3D11SamplerState* samp = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11BlendState* blend = nullptr;
  FLOAT factor[4] = {};
  UINT mask = 0;
  ID3D11DepthStencilState* depth = nullptr;
  UINT stencil = 0;
  ID3D11RasterizerState* raster = nullptr;
  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

  explicit StateGuard(ID3D11DeviceContext* context) : c(context) {
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    c->RSGetViewports(&vpCount, vp);
    c->VSGetShader(&vs, nullptr, nullptr);
    c->PSGetShader(&ps, nullptr, nullptr);
    c->IAGetInputLayout(&il);
    c->IAGetPrimitiveTopology(&topology);
    c->PSGetSamplers(0, 1, &samp);
    c->PSGetShaderResources(0, 1, &srv);
    c->PSGetConstantBuffers(0, 1, &cb);
    c->OMGetBlendState(&blend, factor, &mask);
    c->OMGetDepthStencilState(&depth, &stencil);
    c->RSGetState(&raster);
  }
  ~StateGuard() {
    // THROUGH THE ORIGINAL, never through the vtable. See the bind below.
    getContextProcs(c)->OMSetRenderTargets(
      c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);
    if (vpCount) c->RSSetViewports(vpCount, vp);
    c->VSSetShader(vs, nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);
    c->IASetInputLayout(il);
    c->IASetPrimitiveTopology(topology);
    c->PSSetSamplers(0, 1, &samp);
    c->PSSetShaderResources(0, 1, &srv);
    c->PSSetConstantBuffers(0, 1, &cb);
    c->OMSetBlendState(blend, factor, mask);
    c->OMSetDepthStencilState(depth, stencil);
    c->RSSetState(raster);
    for (auto*& rtv : rtvs) release(rtv);
    release(dsv); release(vs); release(ps); release(il);
    release(samp); release(srv); release(cb); release(blend); release(depth);
    release(raster);
  }
};

}  // namespace

float sharpenAmount() {
  static const float amount = [] {
    // Read the way ShadowMultiplier is: the ini through the same profile API,
    // with the environment overriding it for one session. The sentinel tells a
    // missing key from a key someone deliberately set to 0.
    int percent = 0;
    if (const char* path = configPath()) {
      char value[16] = {};
      GetPrivateProfileStringA("Rendering", "Sharpen", "0", value,
                               sizeof(value), path);
      percent = std::atoi(value);
    }
    if (const char* env = std::getenv("ARLAND_SHARPEN"))
      percent = std::atoi(env);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return float(percent) / 100.0f;
  }();
  return amount;
}

// Called from the hooked Present, off the recording threads. Idempotent, and
// one attempt rather than one per frame: without the latch a system with no
// d3dcompiler at all pays two failing LoadLibrary calls and one log line every
// frame for the life of the process.
void sharpenPreload() {
  static bool attempted = false;
  if (!sharpenEnabled() || g_compiler || attempted)
    return;
  attempted = true;
  g_compiler = LoadLibraryA("d3dcompiler_47.dll");
  if (!g_compiler)
    g_compiler = LoadLibraryA("d3dcompiler.dll");
  if (!g_compiler)
    log("SHARPEN: no d3dcompiler; the pass cannot be built");
}

bool sharpenEnabled() {
  return sharpenAmount() > 0.0f;
}

bool sharpenApply(ID3D11DeviceContext* ctx, ID3D11Texture2D* target) {
  const float amount = sharpenAmount();
  if (amount <= 0.0f || !ctx || !target || t_inSharpen)
    return false;

  D3D11_TEXTURE2D_DESC td = {};
  target->GetDesc(&td);
  if (td.SampleDesc.Count != 1 || !td.Width || !td.Height)
    return false;

  ID3D11Device* device = nullptr;
  target->GetDevice(&device);
  if (!device)
    return false;

  std::lock_guard<atfix::mutex> guard(g_mutex);
  if (!acceptsDevice(device)) {
    release(device);
    return false;
  }
  t_inSharpen = true;
  bool ran = false;
  if (init(device) && ensureScratch(device, td)) {
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = concrete(td.Format);
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(device->CreateRenderTargetView(target, &rd, &rtv)) && rtv) {
      ctx->CopyResource(g_scratch, target);   // read from here, write to target

      D3D11_MAPPED_SUBRESOURCE mapped = {};
      const HRESULT mapResult =
        ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
      if (FAILED(mapResult)) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed))
          log("SHARPEN: constant-buffer update failed (hr=0x", std::hex,
              uint32_t(mapResult), std::dec, "); pass skipped");
      } else {
        Params p = {};
        p.peak = kPeakFloor + amount * (kPeakCeiling - kPeakFloor);
        std::memcpy(mapped.pData, &p, sizeof(p));
        ctx->Unmap(g_cb, 0);
        StateGuard restore(ctx);
        // BOUND THROUGH THE UNHOOKED ORIGINAL, which is what SMAA does and for
        // a reason this pass learned the hard way.
        //
        // ctx->OMSetRenderTargets goes through the mod's own detour, and that
        // detour tells supersampling which target is bound. Binding a scratch
        // here therefore told it the composite was no longer bound, so the
        // composite's viewport correction did not fire on the draw that
        // followed and the engine drew its 3840x2160 scene 1:1 into a
        // 2560x1440 target -- a cropped picture, in an Escha run on 2026-08-08.
        // The pass has always done this; it only became visible once sharpening
        // started running inside the downscale, where the composite marker is
        // live.
        getContextProcs(ctx)->OMSetRenderTargets(ctx, 1, &rtv, nullptr);
        D3D11_VIEWPORT vp = {};
        vp.Width = float(td.Width);
        vp.Height = float(td.Height);
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(g_vs, nullptr, 0);
        ctx->PSSetShader(g_ps, nullptr, 0);
        ctx->PSSetSamplers(0, 1, &g_sampler);
        ctx->PSSetShaderResources(0, 1, &g_scratchSRV);
        ctx->PSSetConstantBuffers(0, 1, &g_cb);
        const FLOAT factor[4] = { 0, 0, 0, 0 };
        ctx->OMSetBlendState(g_blend, factor, 0xffffffff);
        ctx->OMSetDepthStencilState(g_depth, 0);
        ctx->RSSetState(g_raster);
        ctx->Draw(3, 0);
        ran = true;
      }
    }
    release(rtv);
  }
  t_inSharpen = false;
  release(device);

  static std::atomic<bool> announced{false};
  if (ran && !announced.exchange(true, std::memory_order_relaxed))
    log("SHARPEN: active at ", std::dec, int(amount * 100.0f + 0.5f), "% on ",
        td.Width, "x", td.Height, " (after edge smoothing when that is on, and"
        " on its own when it is not)");
  return ran;
}

}  // namespace atfix
