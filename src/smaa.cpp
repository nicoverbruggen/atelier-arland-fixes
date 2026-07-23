// SPDX-License-Identifier: MIT
//
// SMAA (Enhanced Subpixel Morphological Anti-Aliasing, Jimenez et al.) as a
// full-frame post-process applied to the swap chain's back buffer just before
// Present. Unlike MSAA, SMAA works on the finished image, so it smooths edges
// MSAA cannot — texture-interior alpha-test cutouts such as the character
// costume trim — as well as ordinary silhouettes. It runs the standard three
// passes (edge detection -> blending-weight calculation -> neighborhood
// blending) with the reference shader and its two precomputed lookup textures.
//
// The reference shader and the AreaTex/SearchTex data are vendored under
// vendor/smaa/ (MIT). Shaders are compiled at runtime via d3dcompiler, matching
// the mod's existing runtime-shader pattern. Applied at Present, SMAA also
// touches the composited UI (a mild softening of text), the same trade-off the
// Atelier Graphics Tweak's SMAA made; a pre-UI injection point is a later
// refinement.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "smaa.h"
#include "smaa_shader.h"                 // kSmaaReferenceHlsl
#include "../vendor/smaa/AreaTex.h"      // areaTexBytes, AREATEX_WIDTH/HEIGHT
#include "../vendor/smaa/SearchTex.h"    // searchTexBytes, SEARCHTEX_WIDTH/HEIGHT
#include "config.h"
#include "log.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

using PFN_D3DCompile = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR,
  const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
  ID3DBlob**, ID3DBlob**);

// SMAA constant buffer (matches the vendored wrapper's SMAAShaderConstants).
struct SmaaConstants {
  float subsampleIndices[4] = {0, 0, 0, 0};
  float rtMetrics[4] = {0, 0, 0, 0};   // 1/w, 1/h, w, h
  float blendFactor = 1.0f;
  float threshold = 0.0f;
  float maxSearchSteps = 0.0f;
  float maxSearchStepsDiag = 0.0f;
  float cornerRounding = 0.0f;
  float padding[3] = {0, 0, 0};
};

// The thin entry-point wrappers around the reference SMAA functions, appended
// after the reference shader in the compile unit. Register layout follows the
// vendored SMAAWrapper.hlsl: s0 linear, s1 point; t0 area, t1 search, t2 color,
// t3 colorGamma, t8 edges, t9 blend; b0 the constants.
const char* kSmaaWrapper = R"WRAP(
Texture2D areaTex   : register(t0);
Texture2D searchTex : register(t1);
Texture2D colorTex  : register(t2);
Texture2D colorTexGamma : register(t3);
Texture2D edgesTex  : register(t8);
Texture2D blendTex  : register(t9);

void EdgeVS(float4 position : POSITION,
            out float4 svPosition : SV_POSITION,
            inout float2 texcoord : TEXCOORD0,
            out float4 offset[3] : TEXCOORD1) {
  svPosition = position;
  SMAAEdgeDetectionVS(texcoord, offset);
}
float4 EdgePS(float4 position : SV_POSITION,
              float2 texcoord : TEXCOORD0,
              float4 offset[3] : TEXCOORD1) : SV_TARGET {
  return float4(SMAAColorEdgeDetectionPS(texcoord, offset, colorTexGamma), 0.0, 0.0);
}

void WeightVS(float4 position : POSITION,
              out float4 svPosition : SV_POSITION,
              inout float2 texcoord : TEXCOORD0,
              out float2 pixcoord : TEXCOORD1,
              out float4 offset[3] : TEXCOORD2) {
  svPosition = position;
  SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
}
float4 WeightPS(float4 position : SV_POSITION,
                float2 texcoord : TEXCOORD0,
                float2 pixcoord : TEXCOORD1,
                float4 offset[3] : TEXCOORD2) : SV_TARGET {
  return SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset,
    edgesTex, areaTex, searchTex, g_SMAA.subsampleIndices);
}

void BlendVS(float4 position : POSITION,
             out float4 svPosition : SV_POSITION,
             inout float2 texcoord : TEXCOORD0,
             out float4 offset : TEXCOORD1) {
  svPosition = position;
  SMAANeighborhoodBlendingVS(texcoord, offset);
}
float4 BlendPS(float4 position : SV_POSITION,
               float2 texcoord : TEXCOORD0,
               float4 offset : TEXCOORD1) : SV_TARGET {
  return SMAANeighborhoodBlendingPS(texcoord, offset, colorTex, blendTex);
}

// Debug: visualize the edge-detection output (red = horizontal edge, green =
// vertical) over a dimmed scene, so we can see what SMAA is flagging.
float4 DebugEdgesPS(float4 position : SV_POSITION,
                    float2 texcoord : TEXCOORD0,
                    float4 offset[3] : TEXCOORD1) : SV_TARGET {
  float2 e = edgesTex.Sample(LinearSampler, texcoord).rg;
  float3 scene = colorTex.Sample(LinearSampler, texcoord).rgb * 0.25;
  return float4(scene + float3(e, 0.0), 1.0);
}

// Debug: visualize the blend-weight output (pass 2), amplified. Black means the
// weight calculation produced nothing (area/search lookup problem); visible
// colour along edges means weights are being produced and pass 3 is the issue.
float4 DebugWeightsPS(float4 position : SV_POSITION,
                      float2 texcoord : TEXCOORD0,
                      float4 offset[3] : TEXCOORD1) : SV_TARGET {
  float4 w = blendTex.Sample(LinearSampler, texcoord);
  float3 scene = colorTex.Sample(LinearSampler, texcoord).rgb * 0.15;
  return float4(scene + saturate(w.rgb + w.a) * 3.0, 1.0);
}
)WRAP";

// SMAA.hlsl (HLSL_4 path) declares LinearSampler/PointSampler itself, so we do
// not; they auto-assign to s0/s1 in declaration order, which the Present-time
// binding (linear, point) matches.
const char* kSmaaPrefix =
  "#define SMAA_HLSL_4_1 1\n"
  "#define SMAA_PRESET_ULTRA 1\n"
  "struct SMAAShaderConstants { float4 subsampleIndices; float4 rt_metrics;"
  " float blendFactor; float threshld; float maxSearchSteps;"
  " float maxSearchStepsDiag; float cornerRounding;"
  " float padding0; float padding1; float padding2; };\n"
  "cbuffer SMAAGlobals : register(b0) { SMAAShaderConstants g_SMAA; }\n"
  "#define SMAA_RT_METRICS g_SMAA.rt_metrics\n";

// ---- resource state --------------------------------------------------------
std::atomic<bool> g_init{false};
bool g_broken = false;
UINT g_width = 0, g_height = 0;

ID3D11VertexShader* g_edgeVS = nullptr;
ID3D11VertexShader* g_weightVS = nullptr;
ID3D11VertexShader* g_blendVS = nullptr;
ID3D11PixelShader*  g_edgePS = nullptr;
ID3D11PixelShader*  g_weightPS = nullptr;
ID3D11PixelShader*  g_blendPS = nullptr;
ID3D11PixelShader*  g_debugPS = nullptr;      // edges
ID3D11PixelShader*  g_debugWeightPS = nullptr; // weights

// 0 = off, 1 = show edges, 2 = show blend weights.
int smaaDebugLevel() {
  static const int level = [] {
    const char* v = std::getenv("ARLAND_SMAA_DEBUG");
    return v ? std::atoi(v) : 0;
  }();
  return level;
}
ID3D11InputLayout*  g_layout = nullptr;
ID3D11Buffer*       g_quad = nullptr;
ID3D11Buffer*       g_cb = nullptr;
ID3D11SamplerState* g_linear = nullptr;
ID3D11SamplerState* g_point = nullptr;
ID3D11BlendState*   g_blendState = nullptr;
ID3D11DepthStencilState* g_depthState = nullptr;
ID3D11RasterizerState*   g_raster = nullptr;
ID3D11ShaderResourceView* g_areaSRV = nullptr;
ID3D11ShaderResourceView* g_searchSRV = nullptr;

// Per-size targets, recreated if the back buffer size changes.
ID3D11Texture2D* g_sceneTex = nullptr;
ID3D11ShaderResourceView* g_sceneSRV = nullptr;
ID3D11Texture2D* g_edgesTex = nullptr;
ID3D11RenderTargetView* g_edgesRTV = nullptr;
ID3D11ShaderResourceView* g_edgesSRV = nullptr;
ID3D11Texture2D* g_weightTex = nullptr;
ID3D11RenderTargetView* g_weightRTV = nullptr;
ID3D11ShaderResourceView* g_weightSRV = nullptr;

template <typename T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool compile(PFN_D3DCompile D3DCompile, const char* entry, const char* target,
             ID3DBlob** blob) {
  std::string src;
  src.reserve(sizeof(kSmaaReferenceHlsl) + 4096);
  src += kSmaaPrefix;
  src += kSmaaReferenceHlsl;
  src += kSmaaWrapper;
  ID3DBlob* err = nullptr;
  const HRESULT hr = D3DCompile(src.data(), src.size(), "smaa", nullptr,
    nullptr, entry, target, 0, 0, blob, &err);
  if (FAILED(hr)) {
    log("SMAA compile failed entry=", entry, " hr=0x", std::hex, hr,
      std::dec, err ? " : " : "",
      err ? static_cast<const char*>(err->GetBufferPointer()) : "");
    if (err) err->Release();
    return false;
  }
  if (err) err->Release();
  return true;
}

bool initShared(ID3D11Device* dev) {
  HMODULE comp = LoadLibraryA("d3dcompiler_47.dll");
  if (!comp) comp = LoadLibraryA("d3dcompiler.dll");
  if (!comp) { log("SMAA: no d3dcompiler"); return false; }
  auto D3DCompile = reinterpret_cast<PFN_D3DCompile>(
    GetProcAddress(comp, "D3DCompile"));
  if (!D3DCompile) { log("SMAA: no D3DCompile"); return false; }

  ID3DBlob* evs = nullptr; ID3DBlob* wvs = nullptr; ID3DBlob* bvs = nullptr;
  ID3DBlob* eps = nullptr; ID3DBlob* wps = nullptr; ID3DBlob* bps = nullptr;
  bool ok = compile(D3DCompile, "EdgeVS", "vs_4_1", &evs) &&
    compile(D3DCompile, "WeightVS", "vs_4_1", &wvs) &&
    compile(D3DCompile, "BlendVS", "vs_4_1", &bvs) &&
    compile(D3DCompile, "EdgePS", "ps_4_1", &eps) &&
    compile(D3DCompile, "WeightPS", "ps_4_1", &wps) &&
    compile(D3DCompile, "BlendPS", "ps_4_1", &bps);
  if (ok && smaaDebugLevel() > 0) {
    ID3DBlob* dps = nullptr;
    if (compile(D3DCompile, "DebugEdgesPS", "ps_4_1", &dps))
      dev->CreatePixelShader(dps->GetBufferPointer(), dps->GetBufferSize(),
        nullptr, &g_debugPS);
    release(dps);
    ID3DBlob* dwp = nullptr;
    if (compile(D3DCompile, "DebugWeightsPS", "ps_4_1", &dwp))
      dev->CreatePixelShader(dwp->GetBufferPointer(), dwp->GetBufferSize(),
        nullptr, &g_debugWeightPS);
    release(dwp);
  }
  if (ok)
    ok = SUCCEEDED(dev->CreateVertexShader(evs->GetBufferPointer(),
          evs->GetBufferSize(), nullptr, &g_edgeVS)) &&
      SUCCEEDED(dev->CreateVertexShader(wvs->GetBufferPointer(),
          wvs->GetBufferSize(), nullptr, &g_weightVS)) &&
      SUCCEEDED(dev->CreateVertexShader(bvs->GetBufferPointer(),
          bvs->GetBufferSize(), nullptr, &g_blendVS)) &&
      SUCCEEDED(dev->CreatePixelShader(eps->GetBufferPointer(),
          eps->GetBufferSize(), nullptr, &g_edgePS)) &&
      SUCCEEDED(dev->CreatePixelShader(wps->GetBufferPointer(),
          wps->GetBufferSize(), nullptr, &g_weightPS)) &&
      SUCCEEDED(dev->CreatePixelShader(bps->GetBufferPointer(),
          bps->GetBufferSize(), nullptr, &g_blendPS));

  // Fullscreen quad (clip xy + uv), input layout from the edge VS.
  if (ok) {
    const D3D11_INPUT_ELEMENT_DESC elems[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
        D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
        D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ok = SUCCEEDED(dev->CreateInputLayout(elems, 2, evs->GetBufferPointer(),
      evs->GetBufferSize(), &g_layout));
  }
  release(evs); release(wvs); release(bvs);
  release(eps); release(wps); release(bps);
  if (!ok) { log("SMAA: shader/layout init failed"); return false; }

  const float quad[] = {
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
  };
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(quad);
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA qd = {}; qd.pSysMem = quad;
  ok = SUCCEEDED(dev->CreateBuffer(&bd, &qd, &g_quad));

  D3D11_BUFFER_DESC cbd = {};
  cbd.ByteWidth = sizeof(SmaaConstants);
  cbd.Usage = D3D11_USAGE_DYNAMIC;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ok = ok && SUCCEEDED(dev->CreateBuffer(&cbd, nullptr, &g_cb));

  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  ok = ok && SUCCEEDED(dev->CreateSamplerState(&sd, &g_linear));
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  ok = ok && SUCCEEDED(dev->CreateSamplerState(&sd, &g_point));

  D3D11_BLEND_DESC bl = {};
  bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  ok = ok && SUCCEEDED(dev->CreateBlendState(&bl, &g_blendState));
  D3D11_DEPTH_STENCIL_DESC ds = {};
  ds.DepthEnable = FALSE;
  ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&ds, &g_depthState));
  D3D11_RASTERIZER_DESC rs = {};
  rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE;
  ok = ok && SUCCEEDED(dev->CreateRasterizerState(&rs, &g_raster));

  // Lookup textures.
  D3D11_TEXTURE2D_DESC atd = {};
  atd.Width = AREATEX_WIDTH; atd.Height = AREATEX_HEIGHT; atd.MipLevels = 1;
  atd.ArraySize = 1; atd.Format = DXGI_FORMAT_R8G8_UNORM;
  atd.SampleDesc.Count = 1; atd.Usage = D3D11_USAGE_IMMUTABLE;
  atd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA ad = {};
  ad.pSysMem = areaTexBytes; ad.SysMemPitch = AREATEX_PITCH;
  ID3D11Texture2D* area = nullptr;
  ok = ok && SUCCEEDED(dev->CreateTexture2D(&atd, &ad, &area));
  if (ok) ok = SUCCEEDED(dev->CreateShaderResourceView(area, nullptr, &g_areaSRV));
  release(area);

  D3D11_TEXTURE2D_DESC std_ = {};
  std_.Width = SEARCHTEX_WIDTH; std_.Height = SEARCHTEX_HEIGHT; std_.MipLevels = 1;
  std_.ArraySize = 1; std_.Format = DXGI_FORMAT_R8_UNORM;
  std_.SampleDesc.Count = 1; std_.Usage = D3D11_USAGE_IMMUTABLE;
  std_.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA sdd = {};
  sdd.pSysMem = searchTexBytes; sdd.SysMemPitch = SEARCHTEX_PITCH;
  ID3D11Texture2D* search = nullptr;
  ok = ok && SUCCEEDED(dev->CreateTexture2D(&std_, &sdd, &search));
  if (ok) ok = SUCCEEDED(dev->CreateShaderResourceView(search, nullptr, &g_searchSRV));
  release(search);

  if (!ok) log("SMAA: shared resource init failed");
  return ok;
}

void releaseSized() {
  release(g_sceneSRV); release(g_sceneTex);
  release(g_edgesSRV); release(g_edgesRTV); release(g_edgesTex);
  release(g_weightSRV); release(g_weightRTV); release(g_weightTex);
}

bool initSized(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt) {
  releaseSized();
  g_width = w; g_height = h;
  auto make = [&](DXGI_FORMAT format, UINT bind, ID3D11Texture2D** tex,
                  ID3D11RenderTargetView** rtv, ID3D11ShaderResourceView** srv) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = format; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, tex))) return false;
    if (srv && FAILED(dev->CreateShaderResourceView(*tex, nullptr, srv)))
      return false;
    if (rtv && FAILED(dev->CreateRenderTargetView(*tex, nullptr, rtv)))
      return false;
    return true;
  };
  const bool ok =
    make(fmt, D3D11_BIND_SHADER_RESOURCE, &g_sceneTex, nullptr, &g_sceneSRV) &&
    make(DXGI_FORMAT_R8G8B8A8_UNORM,
      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
      &g_edgesTex, &g_edgesRTV, &g_edgesSRV) &&
    make(DXGI_FORMAT_R8G8B8A8_UNORM,
      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
      &g_weightTex, &g_weightRTV, &g_weightSRV);
  if (!ok) { log("SMAA: size ", w, "x", h, " target init failed"); releaseSized(); }
  return ok;
}

}  // namespace

bool smaaEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("ARLAND_SMAA");
    if (env) return env[0] != '0';
    return arlandConfigBool("Rendering", "SMAA", true);   // on by default
  }();
  return enabled;
}

namespace {

// Run the three SMAA passes over `color` (a single-sample colour target),
// writing the antialiased result back into it via `colorRTV`. `allowDebug`
// enables the edge/weight debug views (backbuffer path only). Does not own or
// release dev/ctx/color/colorRTV. Returns false on any setup failure.
bool smaaRun(ID3D11Device* dev, ID3D11DeviceContext* ctx,
             ID3D11Texture2D* color, ID3D11RenderTargetView* colorRTV,
             bool allowDebug) {
  D3D11_TEXTURE2D_DESC cd = {};
  color->GetDesc(&cd);
  if (cd.SampleDesc.Count != 1)
    return false;

  if (!g_init.exchange(true)) {
    if (!initShared(dev)) g_broken = true;
    else log("SMAA initialized (", std::dec, cd.Width, "x", cd.Height, ")");
  }
  if (g_broken)
    return false;
  if (cd.Width != g_width || cd.Height != g_height)
    if (!initSized(dev, cd.Width, cd.Height, cd.Format)) { g_broken = true; return false; }

  ctx->CopyResource(g_sceneTex, color);

  SmaaConstants cb;
  cb.rtMetrics[0] = 1.0f / float(cd.Width);
  cb.rtMetrics[1] = 1.0f / float(cd.Height);
  cb.rtMetrics[2] = float(cd.Width);
  cb.rtMetrics[3] = float(cd.Height);
  D3D11_MAPPED_SUBRESOURCE map = {};
  if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
    std::memcpy(map.pData, &cb, sizeof(cb));
    ctx->Unmap(g_cb, 0);
  }

  const UINT stride = 16, offset = 0;
  const float black[4] = {0, 0, 0, 0};
  D3D11_VIEWPORT vp = {0, 0, float(cd.Width), float(cd.Height), 0, 1};
  ctx->IASetInputLayout(g_layout);
  ctx->IASetVertexBuffers(0, 1, &g_quad, &stride, &offset);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  ctx->RSSetViewports(1, &vp);
  ctx->RSSetState(g_raster);
  ctx->OMSetBlendState(g_blendState, nullptr, 0xffffffff);
  ctx->OMSetDepthStencilState(g_depthState, 0);
  ID3D11SamplerState* samplers[2] = {g_linear, g_point};
  ctx->PSSetSamplers(0, 2, samplers);
  ctx->VSSetConstantBuffers(0, 1, &g_cb);
  ctx->PSSetConstantBuffers(0, 1, &g_cb);
  ID3D11ShaderResourceView* nullSRV[10] = {};

  const int debug = allowDebug ? smaaDebugLevel() : 0;

  // Pass 1: edge detection (colorGamma at t3) -> edges.
  ctx->PSSetShaderResources(0, 10, nullSRV);
  ctx->ClearRenderTargetView(g_edgesRTV, black);
  ctx->OMSetRenderTargets(1, &g_edgesRTV, nullptr);
  ID3D11ShaderResourceView* p1[4] = {g_areaSRV, g_searchSRV, g_sceneSRV, g_sceneSRV};
  ctx->PSSetShaderResources(0, 4, p1);
  ctx->VSSetShader(g_edgeVS, nullptr, 0);
  ctx->PSSetShader(g_edgePS, nullptr, 0);
  ctx->Draw(4, 0);

  if (debug == 1 && g_debugPS) {
    ctx->PSSetShaderResources(0, 10, nullSRV);
    ctx->OMSetRenderTargets(1, &colorRTV, nullptr);
    ID3D11ShaderResourceView* dbg[9] = {g_areaSRV, g_searchSRV, g_sceneSRV,
      nullptr, nullptr, nullptr, nullptr, nullptr, g_edgesSRV};
    ctx->PSSetShaderResources(0, 9, dbg);
    ctx->VSSetShader(g_edgeVS, nullptr, 0);
    ctx->PSSetShader(g_debugPS, nullptr, 0);
    ctx->Draw(4, 0);
    ctx->PSSetShaderResources(0, 10, nullSRV);
    return true;
  }

  // Pass 2: blending-weight calculation (edges at t8) -> weights.
  ctx->PSSetShaderResources(0, 10, nullSRV);
  ctx->ClearRenderTargetView(g_weightRTV, black);
  ctx->OMSetRenderTargets(1, &g_weightRTV, nullptr);
  ID3D11ShaderResourceView* p2[9] = {g_areaSRV, g_searchSRV, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, g_edgesSRV};
  ctx->PSSetShaderResources(0, 9, p2);
  ctx->VSSetShader(g_weightVS, nullptr, 0);
  ctx->PSSetShader(g_weightPS, nullptr, 0);
  ctx->Draw(4, 0);

  if (debug == 2 && g_debugWeightPS) {
    ctx->PSSetShaderResources(0, 10, nullSRV);
    ctx->OMSetRenderTargets(1, &colorRTV, nullptr);
    ID3D11ShaderResourceView* dbg[10] = {g_areaSRV, g_searchSRV, g_sceneSRV,
      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, g_weightSRV};
    ctx->PSSetShaderResources(0, 10, dbg);
    ctx->VSSetShader(g_edgeVS, nullptr, 0);
    ctx->PSSetShader(g_debugWeightPS, nullptr, 0);
    ctx->Draw(4, 0);
    ctx->PSSetShaderResources(0, 10, nullSRV);
    return true;
  }

  // Pass 3: neighborhood blending (color t2 + weights t9) -> the colour target.
  ctx->PSSetShaderResources(0, 10, nullSRV);
  ctx->OMSetRenderTargets(1, &colorRTV, nullptr);
  ID3D11ShaderResourceView* p3[10] = {g_areaSRV, g_searchSRV, g_sceneSRV,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, g_weightSRV};
  ctx->PSSetShaderResources(0, 10, p3);
  ctx->VSSetShader(g_blendVS, nullptr, 0);
  ctx->PSSetShader(g_blendPS, nullptr, 0);
  ctx->Draw(4, 0);
  ctx->PSSetShaderResources(0, 10, nullSRV);
  return true;
}

}  // namespace

bool smaaPreUI() {
  static const bool on = [] {
    const char* v = std::getenv("ARLAND_SMAA_PREUI");
    return !v || v[0] != '0';   // default on: match AGT, spare the UI
  }();
  return on;
}

void smaaApply(IDXGISwapChain* swapChain) {
  // Present-time (full-frame) path — used only when pre-UI injection is off.
  if (!smaaEnabled() || smaaPreUI() || !swapChain || g_broken)
    return;
  ID3D11Texture2D* back = nullptr;
  if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) || !back)
    return;
  ID3D11Device* dev = nullptr;
  back->GetDevice(&dev);
  ID3D11DeviceContext* ctx = nullptr;
  if (dev) dev->GetImmediateContext(&ctx);
  ID3D11RenderTargetView* backRTV = nullptr;
  if (dev && ctx)
    dev->CreateRenderTargetView(back, nullptr, &backRTV);
  if (dev && ctx && backRTV)
    smaaRun(dev, ctx, back, backRTV, true);
  release(backRTV); release(back);
  if (ctx) ctx->Release();
  if (dev) dev->Release();
}

void smaaApplySceneColor(ID3D11DeviceContext* ctx, ID3D11Texture2D* color) {
  if (!smaaEnabled() || !smaaPreUI() || !ctx || !color || g_broken)
    return;
  ID3D11Device* dev = nullptr;
  ctx->GetDevice(&dev);
  ID3D11RenderTargetView* rtv = nullptr;
  if (dev)
    dev->CreateRenderTargetView(color, nullptr, &rtv);
  if (dev && rtv)
    smaaRun(dev, ctx, color, rtv, false);
  release(rtv);
  if (dev) dev->Release();
}

}  // namespace atfix
