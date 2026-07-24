// SPDX-License-Identifier: MIT
//
// Battle cut-in contact-blob overlay, split out of sync_fix.cpp. The cut-in has
// no shadow-receiving ground, so instead of a projected caster this composites a
// soft dark ellipse under each cut-in character's screen-space feet just before
// Present. Self-contained: its own runtime-compiled shaders, vertex buffer and
// pipeline state, drawn over the swap-chain back buffer. Opt-in via
// ARLAND_CUTIN_BLOB=1. It reads the core's per-frame VS cb0 snapshot (readCb0Snap,
// declared in sync_internal.h) to recover each character's world transform.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "sync_fix.h"        // cutinDrawContactBlobs, arlandInCinematicBattle, log
#include "sync_internal.h"   // g_immCtx, readCb0Snap, kCbSnapBytes

namespace atfix {

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

}  // namespace atfix
