// SPDX-License-Identifier: MIT
//
// Supersampling: render the whole frame at [Rendering] RenderWidth/Height and
// downscale it once into the DisplayWidth/Height backbuffer at Present. See
// supersample.h for the shape of the redirect; this file owns the render-res
// colour target, the downscale pass, and the one-time setup.
//
// The downscale is a box filter built out of bilinear taps: one bilinear tap
// centred on a source texel corner already averages 2x2 source texels, so a
// tap grid of n = round(scale/2) per axis covers a 2n x 2n box. Integer scales
// 2x and 4x therefore come out as exact 2x2 and 4x4 box filters, which is what
// supersampling wants; other ratios land close.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "config.h"
#include "log.h"
#include "supersample.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

using PFN_D3DCompile = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR,
  const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
  ID3DBlob**, ID3DBlob**);

// Fullscreen triangle from SV_VertexID: no vertex buffer and no input layout,
// so the pass needs nothing of the game's IA state.
const char* kDownscaleHlsl = R"HLSL(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
  VSOut o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return o;
}

cbuffer Params : register(b0) {
  float2 texel;    // 1 / render size
  float  taps;     // bilinear taps per axis
  float  padding;
};

Texture2D    src  : register(t0);
SamplerState samp : register(s0);

float4 PSMain(VSOut i) : SV_TARGET {
  int n = (int) taps;
  float centre = (taps - 1.0) * 0.5;
  float3 sum = 0.0;
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      float2 offset = (float2(x, y) - centre) * 2.0 * texel;
      sum += src.SampleLevel(samp, i.uv + offset, 0).rgb;
    }
  }
  return float4(sum / (taps * taps), 1.0);
}
)HLSL";

struct DownscaleParams {
  float texel[2] = {0.0f, 0.0f};
  float taps = 1.0f;
  float padding = 0.0f;
};

template <typename T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

std::atomic<bool> g_active{false};
bool g_broken = false;

// Identity only, never dereferenced: the backbuffer outlives the swap chain we
// took it from, and holding a reference to it would pin the chain.
const void* g_backbuffer = nullptr;

// How many render-target views the engine asked for over the backbuffer. Zero
// by the first Present means this build does NOT render straight into the
// backbuffer, so the redirect never engaged and the downscale has nothing of
// the frame in it; that is reported rather than left to look like a black
// screen. Confirmed non-zero on Rorona; the other two report themselves.
std::atomic<uint32_t> g_redirects{0};

UINT g_renderWidth = 0, g_renderHeight = 0;
UINT g_displayWidth = 0, g_displayHeight = 0;
DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;

ID3D11Texture2D* g_color = nullptr;
ID3D11ShaderResourceView* g_colorSRV = nullptr;
ID3D11RenderTargetView* g_backRTV = nullptr;

ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11BlendState* g_blendState = nullptr;
ID3D11DepthStencilState* g_depthState = nullptr;
ID3D11RasterizerState* g_raster = nullptr;

void releaseAll() {
  release(g_colorSRV); release(g_color); release(g_backRTV);
  release(g_vs); release(g_ps); release(g_cb); release(g_sampler);
  release(g_blendState); release(g_depthState); release(g_raster);
}

bool compile(PFN_D3DCompile D3DCompile, const char* entry, const char* target,
             ID3DBlob** blob) {
  ID3DBlob* err = nullptr;
  const HRESULT hr = D3DCompile(kDownscaleHlsl, std::strlen(kDownscaleHlsl),
    "ssaa-downscale", nullptr, nullptr, entry, target, 0, 0, blob, &err);
  if (FAILED(hr)) {
    log("SSAA compile failed entry=", entry, " hr=0x", std::hex, hr, std::dec,
        err ? " : " : "",
        err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    if (err) err->Release();
    return false;
  }
  if (err) err->Release();
  return true;
}

bool initPass(ID3D11Device* device) {
  HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
  if (!compiler) compiler = LoadLibraryA("d3dcompiler.dll");
  if (!compiler) { log("SSAA: no d3dcompiler"); return false; }
  auto D3DCompile = reinterpret_cast<PFN_D3DCompile>(
    GetProcAddress(compiler, "D3DCompile"));
  if (!D3DCompile) { log("SSAA: no D3DCompile"); return false; }

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

  D3D11_BUFFER_DESC cb = {};
  cb.ByteWidth = sizeof(DownscaleParams);
  cb.Usage = D3D11_USAGE_DYNAMIC;
  cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device->CreateBuffer(&cb, nullptr, &g_cb)))
    return false;

  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(device->CreateSamplerState(&sd, &g_sampler)))
    return false;

  // Opaque, depth-less, scissor-less: the pass must not inherit whatever the
  // engine left bound at the end of the frame.
  D3D11_BLEND_DESC bd = {};
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(device->CreateBlendState(&bd, &g_blendState)))
    return false;

  D3D11_DEPTH_STENCIL_DESC dd = {};
  dd.DepthEnable = FALSE;
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
  if (FAILED(device->CreateDepthStencilState(&dd, &g_depthState)))
    return false;

  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  if (FAILED(device->CreateRasterizerState(&rd, &g_raster)))
    return false;

  return true;
}

// Bilinear taps per axis for the configured ratio: each tap covers 2x2 source
// texels, so n taps cover 2n. Clamped so an extreme ratio cannot make the pass
// expensive.
float downscaleTaps() {
  const float scale = float(g_renderHeight) / float(g_displayHeight);
  float taps = scale * 0.5f + 0.5f;   // round(scale / 2)
  if (taps < 1.0f) taps = 1.0f;
  if (taps > 4.0f) taps = 4.0f;
  return float(int(taps));
}

}  // namespace

bool ssaaRequested() {
  static const bool requested = [] {
    UINT renderWidth = 0, renderHeight = 0;
    if (!renderResolution(&renderWidth, &renderHeight))
      return false;
    UINT displayWidth = 0, displayHeight = 0;
    if (!displayResolution(&displayWidth, &displayHeight))
      return true;   // display falls back to the game's own, compared later
    return renderWidth > displayWidth || renderHeight > displayHeight;
  }();
  return requested;
}

bool ssaaActive() {
  return g_active.load(std::memory_order_relaxed);
}

ID3D11Texture2D* ssaaRedirectRenderTargetView(
    ID3D11Resource* resource, const D3D11_RENDER_TARGET_VIEW_DESC* desc) {
  if (!resource || !g_backbuffer || !ssaaActive())
    return nullptr;
  ID3D11Texture2D* texture = nullptr;
  if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))) || !texture)
    return nullptr;
  const bool isBackbuffer = static_cast<const void*>(texture) == g_backbuffer;
  texture->Release();
  if (!isBackbuffer)
    return nullptr;

  const uint32_t seen = g_redirects.fetch_add(1, std::memory_order_relaxed);
  if (desc && desc->Format != DXGI_FORMAT_UNKNOWN && desc->Format != g_format) {
    if (seen < 4)
      log("SSAA backbuffer redirect DECLINED: view format ", std::dec,
          desc->Format, " differs from backbuffer format ", g_format);
    return nullptr;
  }
  if (seen < 4)
    log("SSAA backbuffer render target redirected to ", std::dec,
        g_renderWidth, "x", g_renderHeight);
  g_color->AddRef();
  return g_color;
}

ID3D11Texture2D* ssaaAcquireColor() {
  // Until a redirect has actually happened the frame is still going to the
  // backbuffer, and callers wanting "where the finished frame is" must be told
  // so rather than handed an empty target.
  if (!ssaaActive() || !g_color ||
      !g_redirects.load(std::memory_order_relaxed))
    return nullptr;
  g_color->AddRef();
  return g_color;
}

void ssaaNoteSwapChain(IDXGISwapChain* swapChain) {
  static std::atomic<bool> noted{false};
  if (!swapChain || !ssaaRequested() || noted.exchange(true))
    return;

  ID3D11Texture2D* back = nullptr;
  if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) || !back)
    return;
  D3D11_TEXTURE2D_DESC backDesc = {};
  back->GetDesc(&backDesc);

  UINT renderWidth = 0, renderHeight = 0;
  if (!renderResolution(&renderWidth, &renderHeight) ||
      (renderWidth <= backDesc.Width && renderHeight <= backDesc.Height)) {
    // A render resolution no larger than the backbuffer is not supersampling;
    // the existing single-resolution path already covers it.
    back->Release();
    return;
  }

  ID3D11Device* device = nullptr;
  back->GetDevice(&device);
  if (!device) { back->Release(); return; }

  g_displayWidth = backDesc.Width;
  g_displayHeight = backDesc.Height;
  g_renderWidth = renderWidth;
  g_renderHeight = renderHeight;
  g_format = backDesc.Format;

  D3D11_TEXTURE2D_DESC colorDesc = {};
  colorDesc.Width = renderWidth;
  colorDesc.Height = renderHeight;
  colorDesc.MipLevels = 1;
  colorDesc.ArraySize = 1;
  colorDesc.Format = backDesc.Format;
  colorDesc.SampleDesc.Count = 1;
  colorDesc.Usage = D3D11_USAGE_DEFAULT;
  colorDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  bool ok = SUCCEEDED(device->CreateTexture2D(&colorDesc, nullptr, &g_color)) &&
    SUCCEEDED(device->CreateShaderResourceView(g_color, nullptr, &g_colorSRV)) &&
    // Created here, while the redirect is still disarmed, so this one view over
    // the real backbuffer survives; every later one is redirected.
    SUCCEEDED(device->CreateRenderTargetView(back, nullptr, &g_backRTV)) &&
    initPass(device);

  if (ok) {
    g_backbuffer = static_cast<const void*>(back);
    g_active.store(true, std::memory_order_relaxed);
    log("Supersampling ", std::dec, renderWidth, "x", renderHeight,
        " -> ", backDesc.Width, "x", backDesc.Height,
        " (", int(downscaleTaps()), " taps/axis, format ", backDesc.Format, ")");
  } else {
    releaseAll();
    log("Supersampling setup failed; rendering at the backbuffer resolution");
  }

  device->Release();
  back->Release();
}

void ssaaDownscale(IDXGISwapChain* swapChain) {
  if (!ssaaActive() || g_broken || !swapChain)
    return;

  // One-shot verdict on the first presented frame: whether this build really
  // renders into the backbuffer (the assumption the redirect rests on). Cheap,
  // and it is the line to read when checking a game the feature is new to.
  static std::atomic<bool> reported{false};
  if (!reported.exchange(true, std::memory_order_relaxed)) {
    const uint32_t redirects = g_redirects.load(std::memory_order_relaxed);
    if (redirects)
      log("SSAA active: ", std::dec, redirects,
          " backbuffer render target(s) redirected to ",
          g_renderWidth, "x", g_renderHeight);
    else
      log("SSAA INACTIVE: this build never bound the backbuffer as a render"
          " target, so the frame is not being supersampled. Clear"
          " [Rendering] RenderWidth/RenderHeight for this game.");
  }
  if (!g_redirects.load(std::memory_order_relaxed))
    return;   // nothing of the frame is in our target; leave the backbuffer be

  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  g_color->GetDevice(&device);
  if (device) device->GetImmediateContext(&context);
  if (!context) { if (device) device->Release(); return; }

  DownscaleParams params;
  params.texel[0] = 1.0f / float(g_renderWidth);
  params.texel[1] = 1.0f / float(g_renderHeight);
  params.taps = downscaleTaps();
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (SUCCEEDED(context->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    std::memcpy(mapped.pData, &params, sizeof(params));
    context->Unmap(g_cb, 0);
  }

  // Bind the backbuffer first: that unbinds the render target the game left
  // bound, which is the very texture we are about to sample.
  context->OMSetRenderTargets(1, &g_backRTV, nullptr);
  const D3D11_VIEWPORT viewport = {
    0.0f, 0.0f, float(g_displayWidth), float(g_displayHeight), 0.0f, 1.0f };
  context->RSSetViewports(1, &viewport);
  context->RSSetState(g_raster);
  context->OMSetBlendState(g_blendState, nullptr, 0xffffffff);
  context->OMSetDepthStencilState(g_depthState, 0);
  context->IASetInputLayout(nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(g_vs, nullptr, 0);
  context->PSSetShader(g_ps, nullptr, 0);
  context->PSSetConstantBuffers(0, 1, &g_cb);
  context->PSSetSamplers(0, 1, &g_sampler);
  context->PSSetShaderResources(0, 1, &g_colorSRV);
  context->Draw(3, 0);

  ID3D11ShaderResourceView* none = nullptr;
  context->PSSetShaderResources(0, 1, &none);
  context->Release();
  if (device) device->Release();
}

}  // namespace atfix
