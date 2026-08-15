// SPDX-License-Identifier: MIT
//
// Supersampling: render the whole frame at [Rendering] RenderWidth/Height and
// downscale it once into the DisplayWidth/Height backbuffer at Present. See
// supersample.h for the shape of the redirect; this file owns the render-res
// colour target, the downscale pass, and the one-time setup.
//
// The downscale is a box filter: each output pixel averages the source texels
// its own footprint covers, sampled on a grid spread across that footprint.
// At an integer ratio the samples land exactly on texel centres and the result
// is an exact box, which is what supersampling wants; other ratios sample the
// footprint evenly and land close.
//
// This replaced a scheme that placed bilinear taps on texel corners to average
// 2x2 for free. That is a real optimisation, but only for EVEN ratios: at 3x
// the output pixel centre falls on a texel centre rather than a corner, so
// every tap collapsed to a single texel and the filter read four corners of
// the 3x3 block while skipping the middle -- undersampled and soft at once.
// Correctness at every ratio is worth more here than the saved samples, and
// this pass runs once per frame at display resolution.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "config.h"
#include "log.h"
#include "pipeline_state.h"
#include "supersample.h"
#include "../engines/phyre/sync_fix.h"

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
  float2 ratio;    // render size / display size, per axis
  float  samples;  // samples per axis
  float3 padding;
};

Texture2D    src  : register(t0);
SamplerState samp : register(s0);

// Average the source texels this output pixel actually covers.
//
// The footprint of output pixel p spans source texels [p*ratio, (p+1)*ratio].
// i.uv * sourceSize is the CENTRE of that footprint, so backing off half a
// ratio gives its top-left corner; samples are then spread evenly across it.
// For an integer ratio with samples == ratio this lands exactly on texel
// centres and the result is an exact box filter -- for odd ratios as well as
// even ones, which the earlier corner-tap scheme could not do.
float4 PSMain(VSOut i) : SV_TARGET {
  int n = (int) samples;
  float2 sourceSize = 1.0 / texel;
  float2 origin = i.uv * sourceSize - 0.5 * ratio;
  float2 step = ratio / samples;
  float3 sum = 0.0;
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      float2 position = origin + (float2(x, y) + 0.5) * step;
      sum += src.SampleLevel(samp, position * texel, 0).rgb;
    }
  }
  return float4(sum / (samples * samples), 1.0);
}
)HLSL";

// The downscale's constants. The size is load-bearing outside this file: the
// pass maps through the hooked context, so sync_fix.cpp's constant-buffer
// capture sees this buffer, and battle_shadows.cpp's Totori dim-hold table
// matches a buffer by size alone. 32 is a live row in that table. What keeps the
// patch off this buffer is the value predicate: it needs the first float above
// 0.5, and texel[0] is 1/renderWidth. Reordering these fields so a value above
// 0.5 lands at offset 0 arms that write.
struct DownscaleParams {
  float texel[2] = {0.0f, 0.0f};
  float ratio[2] = {1.0f, 1.0f};
  float samples = 1.0f;
  float padding[3] = {0.0f, 0.0f, 0.0f};
};

template <typename T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// True once the pass has built its resources, and never cleared afterwards.
// releaseAll below runs only when setup fails, where this was never set. So
// there is no teardown for a device loss or a swap-chain resize, and g_backRTV
// holds a view over the backbuffer for the process lifetime, which would make a
// ResizeBuffers call fail if one were ever made.
//
// Left that way on purpose, and here is what that rests on. All three games do
// contain a ResizeBuffers call: it sits in PApplication's vtable slot 1, the
// method that both creates and resizes the swap chain, and the resize half is
// reachable only when the chain already exists, so a first invocation creates
// and skips it (rorona-en 0x3daadd, totori-en 0x4beb6d, meruru-en 0x3dcb3d,
// each on the chain cached at [this+0x3018]). What invokes that slot a second
// time was not found. It is reached only through the vtable, so there is no
// call site to point at; the object is a global built before WinMain, no
// window-message handler appears anywhere in the path, and these games have no
// display-settings screen. A runtime probe across the three games never saw the
// call fire.
//
// So the reference is held on "not observed", not on "cannot happen". A
// stand-down path would be new code releasing COM references during a teardown
// nobody has seen, and it may also be load-bearing in the other direction: if
// the game does resize at startup, this reference failing it is what keeps the
// mod's own chosen size rather than the game's.
//
// If a change ever introduces a resize, or a device-loss path, this is the
// assumption it breaks and the first place that has to be handled.
std::atomic<bool> g_active{false};

// Identity only, never dereferenced: g_backRTV holds a reference to this same
// texture for the process lifetime, so the address cannot be recycled under the
// comparison and a second reference here would buy nothing.
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

ID3D11Device* g_ownerDevice = nullptr;

// One device owns everything this pass creates. Handing a second device the
// first one's shaders, states or textures is undefined, so the first device to
// arrive claims the pass and any other is refused. Ported from the Dusk
// project, which carries the same guard in all three of its passes.
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
    log("SSAA: a second D3D11 device reached the shared downscale pass;"
        " refusing it so resources are never bound across devices");
  return false;
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

// Samples per axis: enough to cover every source texel the output pixel spans,
// so a 3x ratio takes 3 and averages all nine rather than four corners of them.
// Rounded up, because covering slightly more than the footprint is a mild blur
// while covering less is aliasing. Clamped at 8, which no supported ratio
// reaches (the render resolution is capped at 8K), so the pass cannot become
// arbitrarily expensive if that ever changes.
// How much the frame is shrunk to fit inside the backbuffer, preserving its
// shape. One number for both axes by construction, and the source of both the
// viewport below and the per-pixel footprint the shader averages over.
float downscaleFit() {
  const float fit = std::min(
    float(g_displayWidth) / float(g_renderWidth),
    float(g_displayHeight) / float(g_renderHeight));
  return fit > 0.0f ? fit : 1.0f;
}

float downscaleSamples() {
  // Derived from the fit, not from the height ratio alone: when the width is
  // the constraining axis the height ratio is the smaller number, and taking
  // the tap count from it undersamples on both axes at once.
  const float scale = 1.0f / downscaleFit();
  float samples = std::ceil(scale);
  if (samples < 1.0f) samples = 1.0f;
  if (samples > 8.0f) samples = 8.0f;
  return samples;
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
    // Two reasons, the same two ssaaNoteSwapChain uses against the real
    // backbuffer. Larger is supersampling. A different SHAPE is a 16:9 frame on
    // a panel that is not, which needs fitting rather than stretching -- and
    // that case is smaller on one axis and equal on the other, so the size test
    // alone answers false and the pass never installs.
    const bool larger =
      renderWidth > displayWidth || renderHeight > displayHeight;
    const bool differentShape =
      uint64_t(renderWidth) * displayHeight !=
      uint64_t(displayWidth) * renderHeight;
    return larger || differentShape;
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

  // Counted only once the redirect is certain to be taken. Downscale at Present
  // gates on this counter, so counting a declined redirect would blit a target
  // the game never rendered into over the finished frame.
  if (desc && desc->Format != DXGI_FORMAT_UNKNOWN && desc->Format != g_format) {
    static std::atomic<bool> reportedDecline{false};
    if (verboseLogging() ||
        !reportedDecline.exchange(true, std::memory_order_relaxed))
      log("SSAA backbuffer redirect DECLINED: view format ", std::dec,
          desc->Format, " differs from backbuffer format ", g_format);
    return nullptr;
  }
  const uint32_t seen = g_redirects.fetch_add(1, std::memory_order_relaxed);
  if (seen < 4 && verboseLogging())
    log("SSAA backbuffer render target redirected to ", std::dec,
        g_renderWidth, "x", g_renderHeight);
  g_color->AddRef();
  return g_color;
}

bool ssaaIsBackbuffer(ID3D11Resource* resource) {
  if (!resource || !g_backbuffer || !ssaaActive())
    return false;
  ID3D11Texture2D* texture = nullptr;
  if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))) || !texture)
    return false;
  const bool isBackbuffer = static_cast<const void*>(texture) == g_backbuffer;
  texture->Release();
  return isBackbuffer;
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
  static std::mutex setupMutex;
  static bool initialized = false;
  if (!swapChain || !ssaaRequested())
    return;
  std::lock_guard<std::mutex> setupGuard(setupMutex);
  if (initialized)
    return;

  ID3D11Texture2D* back = nullptr;
  if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) || !back)
    return;
  D3D11_TEXTURE2D_DESC backDesc = {};
  back->GetDesc(&backDesc);

  UINT renderWidth = 0, renderHeight = 0;
  if (!renderResolution(&renderWidth, &renderHeight)) {
    back->Release();
    return;
  }
  // Two reasons to take this path: rendering larger than the backbuffer
  // (supersampling), or rendering a different SHAPE from it (a 16:9 game on a
  // 16:10 panel), which needs fitting rather than stretching. Same size and
  // same shape needs neither.
  const bool larger =
    renderWidth > backDesc.Width || renderHeight > backDesc.Height;
  const bool differentShape =
    uint64_t(renderWidth) * backDesc.Height !=
    uint64_t(backDesc.Width) * renderHeight;
  if (!larger && !differentShape) {
    back->Release();
    return;
  }

  ID3D11Device* device = nullptr;
  back->GetDevice(&device);
  if (!device) { back->Release(); return; }
  // Claims the device for the pass, since this is where its targets are made.
  if (!acceptsDevice(device)) { device->Release(); back->Release(); return; }

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
    // Publish completion only after every object exists. A failed or unsuitable
    // first chain remains retryable when the game presents another one.
    initialized = true;
    log("FIXES supersampling=initialized render=", std::dec,
        renderWidth, "x", renderHeight,
        " display=", backDesc.Width, "x", backDesc.Height,
        " samples_per_axis=", int(downscaleSamples()),
        " format=", backDesc.Format);
  } else {
    releaseAll();
    log("Supersampling setup failed; rendering at the backbuffer resolution");
  }

  device->Release();
  back->Release();
}

void ssaaDownscale(IDXGISwapChain* swapChain) {
  if (!ssaaActive() || !swapChain)
    return;

  // One-shot verdict on the first presented frame: whether this build really
  // renders into the backbuffer (the assumption the redirect rests on). Cheap,
  // and it is the line to read when checking a game the feature is new to.
  static std::atomic<bool> reported{false};
  const bool firstFrame = !reported.exchange(true, std::memory_order_relaxed);
  if (firstFrame) {
    const uint32_t redirects = g_redirects.load(std::memory_order_relaxed);
    if (redirects)
      log("FIXES supersampling=active redirects=", std::dec, redirects,
          " render=", g_renderWidth, "x", g_renderHeight);
    else
      log("SSAA INACTIVE: this build never bound the backbuffer as a render"
          " target, so the frame is not being supersampled. Clear"
          " [Rendering] RenderWidth/RenderHeight for this game.");
  }
  if (!g_redirects.load(std::memory_order_relaxed))
    return;   // nothing of the frame is in our target; leave the backbuffer be

  // Deliberately the hooked context: this pass relies on re-entering the proxy
  // (see the PSSetShaderResources note below, and AGENTS.md).
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  g_color->GetDevice(&device);
  if (device) device->GetImmediateContext(&context);
  if (!context) { if (device) device->Release(); return; }
  if (!acceptsDevice(device)) { context->Release(); device->Release(); return; }

  // The rest of the picture, a few hundred frames in. "Supersampling is on but
  // the edges are still jagged" cannot be answered from the lines above, because
  // they only say the redirect happened. What decides whether the result is
  // actually supersampled is how much of the target the engine drew into (the
  // viewport it was still using when the frame ended) and how many source texels
  // each output pixel averages. A viewport smaller than the render size means
  // the engine drew a corner of the target and the downscale is magnifying it;
  // taps=1 at a 2x ratio is the exact 2x2 box filter and is correct, but at a
  // larger ratio it is undersampling. Not on the first frame: the first frames
  // are a loading screen drawn at swap-chain size, so "the largest viewport so
  // far" is not yet the answer to what the engine draws the game at, and asking
  // then produces a confident report of a problem that does not exist. The
  // detail line follows verbose logging; the warning below does not, because it
  // names a real problem rather than describing a healthy run.
  static std::atomic<uint32_t> framesSeen{0};
  const uint32_t frame = framesSeen.fetch_add(1, std::memory_order_relaxed);
  if (frame == 300) {
    // The largest viewport the engine has set on ANY context. Sampling the
    // immediate context here reads back nothing, because these games record
    // their frames on deferred contexts -- so this is tracked from the
    // viewport hook instead.
    unsigned int drawnWidth = 0, drawnHeight = 0;
    largestViewportSeen(&drawnWidth, &drawnHeight);
    const float ratio = float(g_renderHeight) / float(g_displayHeight);
    if (verboseLogging())
      log("SSAA frame: render=", std::dec,
          g_renderWidth, "x", g_renderHeight,
          " display=", g_displayWidth, "x", g_displayHeight,
          " ratio=", ratio,
          " samples/axis=", int(downscaleSamples()),
          " largest_viewport=", drawnWidth, "x", drawnHeight);
    if (drawnWidth && (drawnWidth < g_renderWidth ||
                       drawnHeight < g_renderHeight))
      log("SSAA WARNING: the engine never drew at the full render size, so only"
          " part of the target holds the frame. The downscale is magnifying"
          " that part rather than supersampling it.");
  }

  // The pass draws into the fitted rectangle, not the whole backbuffer, so an
  // output pixel covers render-size-over-FITTED-size source texels. The fit
  // preserves the frame's shape, so that is one number for both axes. Measuring
  // against the display size instead understates whichever axis the fit is not
  // constrained by and narrows the filter kernel there; the two agree exactly
  // when the render and display aspects match, which is the ordinary case.
  const float fit = downscaleFit();

  DownscaleParams params;
  params.texel[0] = 1.0f / float(g_renderWidth);
  params.texel[1] = 1.0f / float(g_renderHeight);
  params.ratio[0] = 1.0f / fit;
  params.ratio[1] = 1.0f / fit;
  params.samples = downscaleSamples();
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  const HRESULT mapResult =
    context->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
  if (FAILED(mapResult)) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
      log("SSAA: constant-buffer update failed (hr=0x", std::hex,
          uint32_t(mapResult), std::dec, "); downscale skipped");
    context->Release();
    device->Release();
    return;
  }
  std::memcpy(mapped.pData, &params, sizeof(params));
  context->Unmap(g_cb, 0);

  {
    // Same state discipline as the SMAA passes: everything the downscale binds
    // is captured here and put back when the scope closes (before the context
    // reference is released below), so the context leaves Present exactly as
    // the game left it. The engines record their frames on deferred contexts
    // whose command lists carry their own state, so nothing is known to read
    // the leaked bindings -- but that is the engine's property, not this
    // function's to assume, and smaa.cpp already restores on the same Present
    // path.
    ScopedPipelineState savedState(context);

    // Bind the backbuffer first: that unbinds the render target the game left
    // bound, which is the very texture we are about to sample.
    context->OMSetRenderTargets(1, &g_backRTV, nullptr);

    // Fit the rendered frame inside the backbuffer without changing its shape:
    // the larger of the two scale factors would crop, so the smaller one is
    // used and the remainder becomes bars. When the shapes match this is the
    // whole backbuffer and the clear costs one fill of pixels nothing else
    // writes.
    const float scale = downscaleFit();
    const float fittedWidth = float(g_renderWidth) * scale;
    const float fittedHeight = float(g_renderHeight) * scale;
    const D3D11_VIEWPORT viewport = {
      (float(g_displayWidth) - fittedWidth) * 0.5f,
      (float(g_displayHeight) - fittedHeight) * 0.5f,
      fittedWidth, fittedHeight, 0.0f, 1.0f };
    // The bars have to be painted every frame: nothing else in the pipeline
    // writes those pixels, so whatever the last frame left there would stay.
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    context->ClearRenderTargetView(g_backRTV, black);
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
    // Deliberately the hooked context, not a getContextProcs call. See the
    // post-process exception in AGENTS.md before changing it.
    context->PSSetShaderResources(0, 1, &g_colorSRV);
    context->Draw(3, 0);

    // savedState's destructor runs here and restores the game's own SRV over
    // slot 0, which is what an explicit unbind used to do.
  }
  context->Release();
  if (device) device->Release();
}

}  // namespace atfix
