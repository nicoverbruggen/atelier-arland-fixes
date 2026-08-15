// Derived from Philip Rebohle's atelier-sync-fix and subsequent Map/Unmap
// work by TellowKrinkle; substantially altered for Arland. See LICENSE.
#include <array>
#include <atomic>
#include <deque>
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
#include "../../core/util.h"
#include "../../core/game.h"
#include "../../core/config.h"
#include "../../core/sharpen.h"
#include "../../core/smaa.h"
#include "../../core/supersample.h"
#include "sync_internal.h"
#include "sync_upload_policy.h"
#include "../../core/d3d11_procs.h"

namespace atfix {

// ============================================================================
// sync_fix.cpp: the D3D11 proxy layer. It proxies the device/context vtables and
// implements the CPU shadow-copy sync fix, the resolution and shadow-map-twin
// scaling, and the resource interception those and the cut-in features hook into.
// The pieces that remain here share per-frame proxy state (the constant-buffer
// snapshot cache, the immediate-context pointer, the DeviceProcs/ContextProcs
// vtable dispatch), so they stay in one translation unit. Split-out siblings: the
// vtable dispatch types live in d3d11_procs.h; the cut-in shadow feature (the
// dim/gate constant-buffer patches, the shadow-SRV classifier, and the
// contact-blob overlay) lives in battle_shadows.cpp, wired through
// sync_internal.h; config.cpp and game.cpp own arland-fix.ini and the per-game
// capability matrix. Sections here, in order:
//
//   1. Shadow-map "twin" plumbing: separate mod-owned high-res shadow maps for
//      ShadowMultiplier, redirected onto without touching the engine's own maps.
//   2. Constant-buffer write interception (captureCbMap / captureCbUnmap)
//      feeding the cut-in patches (battle_shadows.cpp) and the
//      shadow-map-clear callback.
//   3. Resolution override + viewport/scissor correction.
//   4. The D3D11 device/context hook implementations that call into 1-3 and the
//      battle_shadows.cpp cut-in entry points.
//   5. hookDevice / hookContext installation.
// ============================================================================


static mutex  g_hookMutex;
static mutex  g_globalMutex;

DeviceProcs   g_deviceProcs;
ContextProcs  g_immContextProcs;
ContextProcs  g_defContextProcs;

constexpr uint32_t HOOK_DEVICE  = (1u << 0);
constexpr uint32_t HOOK_IMM_CTX = (1u << 1);
constexpr uint32_t HOOK_DEF_CTX = (1u << 2);

uint32_t      g_installedHooks = 0u;

// Hot D3D paths can encounter the same failure every frame. Keep the first
// occurrence in a normal log, then sample repeats only when verbose logging was
// explicitly requested. Even a diagnostic log should not grow by one line per
// frame forever.
bool logFirstOrVerbose(std::atomic<uint32_t>& occurrences) {
  const uint32_t seen =
    occurrences.fetch_add(1, std::memory_order_relaxed);
  return seen == 0 ||
    (verboseLogging() && (seen < 16 || seen % 4096 == 0));
}

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
//              VS's `diffuse` at byte 832 (a name collision -- the PS RDEF
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
// absent so it appears in the file for the user. Defined in config.cpp.
bool arlandConfigBool(const char* section, const char* key, bool def);

// ShadowMultiplier (arland-fix.ini [Rendering], default 2): scales
// the engine's two 1024x1024 R24G8 shadow maps. Values 2, 4 and 8 enlarge the
// maps to 2048/4096/8192 (plus the caster viewport, the A->B copy box and the
// receiver's PCF tap scale); anything else, 1 included, keeps vanilla
// behaviour.
// ARLAND_SHADOW_MULTIPLIER overrides the ini. Defined in config.cpp.
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
    static std::atomic<uint32_t> reported{0};
    if (logFirstOrVerbose(reported))
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
    static std::atomic<uint32_t> reported{0};
    if (logFirstOrVerbose(reported))
      log("SHADOWRES twin SRV creation FAILED hr=0x", std::hex, hr);
    std::lock_guard lock(g_twinSrvNegMutex);
    g_twinSrvNegative.insert(reinterpret_cast<uintptr_t>(hostSrv));
    return nullptr;
  }
  hostSrv->SetPrivateDataInterface(IID_ShadowResTwinView, twinSrv);
  return twinSrv;   // creation ref belongs to the caller
}


// The write-path patches consume CPU-side constant-buffer writes (a Map payload
// is only visible between Map and Unmap).
bool cbCaptureEnabled() {
  return cutinShadowsEnabled() || shadowMapResolution() > 1024;
}

mutex g_cbCaptureMutex;
// Keyed by context as well as subresource: the same DYNAMIC buffer can be
// mapped WRITE_DISCARD on the immediate and on a deferred context at once, and
// each map renames to its own CPU region. A key without the context would let
// one context's Unmap patch through the other's still-live pointer, leaving the
// region it meant to patch untouched.
std::map<std::tuple<ID3D11DeviceContext*, ID3D11Resource*, UINT>,
         std::pair<const void*, uint32_t>>
    g_cbCapturePending;

bool isConstantBuffer(ID3D11Resource* resource, D3D11_BUFFER_DESC* desc) {
  ID3D11Buffer* buffer = nullptr;
  if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(&buffer))))
    return false;
  buffer->GetDesc(desc);
  buffer->Release();
  return (desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER) && desc->ByteWidth >= 16;
}

// The game writes cbuffers via Map(WRITE_DISCARD)/Unmap; the CPU payload is
// only valid between Map and Unmap. Track the mapped pointer per context and
// subresource so captureCbUnmap can patch it right before the real Unmap.
void captureCbMap(ID3D11DeviceContext* ctx, ID3D11Resource* resource, UINT sub,
                  const D3D11_MAPPED_SUBRESOURCE* mapped) {
  if (!cbCaptureEnabled() || !resource || !mapped || !mapped->pData)
    return;
  D3D11_BUFFER_DESC desc = {};
  if (!isConstantBuffer(resource, &desc))
    return;
  std::lock_guard lock(g_cbCaptureMutex);
  g_cbCapturePending[{ctx, resource, sub}] = {mapped->pData, desc.ByteWidth};
}

void captureCbUnmap(ID3D11DeviceContext* ctx, ID3D11Resource* resource,
                    UINT sub) {
  if (!cbCaptureEnabled())
    return;
  std::pair<const void*, uint32_t> pending{nullptr, 0};
  {
    std::lock_guard lock(g_cbCaptureMutex);
    auto it = g_cbCapturePending.find({ctx, resource, sub});
    if (it != g_cbCapturePending.end()) {
      pending = it->second;
      g_cbCapturePending.erase(it);
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
}

std::atomic<ID3D11DeviceContext*> g_immCtx{nullptr};

// One context does not mean one thread. A Rorona session traced with the
// CTX_THREAD line in smaaDrawBoundary showed the immediate context driven from
// four threads in turn (604, 768, 856, back to 604) while the single deferred
// context stayed on one. So "the recording thread" is not a stable identity,
// and nothing here may assume that state written during one draw is read back
// on the same thread.
//
// It is safe, and the reason is the engine's rather than ours: D3D11 requires
// the application to guarantee one thread at a time per context, so the game
// already synchronises the handoff, and whatever it uses to do that publishes
// our globals along with its own. What this does rule out is reasoning of the
// form "only one thread records, so this needs no synchronisation" -- that was
// assumed in several places before it was measured, and it was wrong.

// A shadow-map clear opens a battle frame's shadow pass, which is where the
// cut-in holds need this frame's battle state. The callback is in
// battle_shadow_restore.cpp.
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

// What a render target IS, recorded when the mod resizes it, so nothing has to
// work it out again from its dimensions later.
//
// Sizes cannot answer this question reliably. At exactly 2x supersampling
// main/2 == the display size, so "half the render target" and "the size the
// game asked for" are the same number and a size test cannot tell a half-res
// blur target from a full-size one. Every such collision this project has hit
// -- the black conversations, the scene drawn into a quarter of the frame --
// is that ambiguity resolving the wrong way. The mod knows the answer at the
// moment it resizes the texture; this writes it down instead of guessing later.
static const GUID IID_ResolutionRole =
  {0x9a3c17e4,0x5b62,0x4f0a,{0xb1,0x77,0x2e,0x64,0x9d,0x3f,0x8c,0x21}};
enum ResolutionRole : UINT { RoleNone = 0, RoleMain = 1, RoleHalf = 2 };

UINT resolutionRole(ID3D11Resource* resource) {
  if (!resource)
    return RoleNone;
  UINT role = RoleNone;
  UINT size = sizeof(role);
  if (FAILED(resource->GetPrivateData(IID_ResolutionRole, &size, &role)))
    return RoleNone;
  return role;
}

static const GUID IID_WireframeState = {0xe2728d99,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};
static const GUID IID_DialogSnapshotResource = {0xe2728d9c,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};
static const GUID IID_DialogScaledVertexBuffer = {0xe2728d9d,0x9fdd,0x40d0,{0x87,0xa8,0x09,0xb6,0x2d,0xf3,0x14,0x9a}};


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

// configPath / arlandConfigBool / shadowMapResolution / configuredResolution
// moved to config.cpp (arland-fix.ini access). applyResolutionOverride stays
// here because it mutates the resolution globals below.

// Whether swap-chain creation has to be intercepted for the resolution policy
// alone, with no other feature asking for it. This mirrors the two gates in
// applyResolutionOverride below: the render size decides whether the game's own
// swap-chain size is recorded, and the display size decides whether the chain is
// rewritten. Keep the two in step. A blank ini still answers true, because
// displayResolution falls back to the desktop mode, which is what moves a fresh
// install off the game's 720p default.
bool resolutionOverrideNeeded() {
  UINT width = 0;
  UINT height = 0;
  return displayResolution(&width, &height) ||
    renderResolution(&width, &height);
}

namespace {

// Give a window a client area of exactly this size, and centre what comes out.
// The frame is measured from the window's own style rather than assumed: a
// borderless window and a titled one need different amounts. Centred because
// the window usually moves by a few hundred pixels here, and resizing about the
// top-left corner leaves it half off the screen.
void resizeClientArea(HWND window, UINT width, UINT height) {
  if (!window || !width || !height)
    return;
  RECT frame = { 0, 0, LONG(width), LONG(height) };
  const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
  const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
  if (!AdjustWindowRectEx(&frame, DWORD(style), GetMenu(window) != nullptr,
                          DWORD(exStyle)))
    return;
  const int frameWidth = frame.right - frame.left;
  const int frameHeight = frame.bottom - frame.top;

  int x = 0;
  int y = 0;
  RECT work = { };
  if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
    x = int(work.left) + (int(work.right - work.left) - frameWidth) / 2;
    y = int(work.top) + (int(work.bottom - work.top) - frameHeight) / 2;
    // A window bigger than the work area centres to a negative origin, which
    // hides its title bar behind the panel and leaves it unmovable.
    if (x < int(work.left))
      x = int(work.left);
    if (y < int(work.top))
      y = int(work.top);
  }
  SetWindowPos(window, nullptr, x, y, frameWidth, frameHeight,
               SWP_NOZORDER | SWP_NOACTIVATE);
  log("Windowed: client area set to ", std::dec, width, "x", height,
      ", window ", frameWidth, "x", frameHeight, " at ", x, ",", y);
}

}  // namespace

bool applyResolutionOverride(DXGI_SWAP_CHAIN_DESC* pDesc) {
  if (!pDesc)
    return false;
  // Remember the size the game itself asked for whenever ANY resolution is
  // configured, not only when the swap chain is rewritten: supersampling with
  // no display override leaves the chain alone but still needs the main-target
  // detection in CreateTexture2D to recognize the game's own size. With nothing
  // configured this stays zero and that detection keeps its old, narrower
  // signal, so an unconfigured install behaves exactly as before.
  UINT renderWidth = 0;
  UINT renderHeight = 0;
  if (renderResolution(&renderWidth, &renderHeight)) {
    g_originalSwapWidth.store(
      pDesc->BufferDesc.Width, std::memory_order_relaxed);
    g_originalSwapHeight.store(
      pDesc->BufferDesc.Height, std::memory_order_relaxed);
  }
  UINT width = 0;
  UINT height = 0;
  if (!displayResolution(&width, &height))
    return false;
  // The swap effect is left as the game asked for it, and that has a cost worth
  // knowing before someone changes it here. These games ask for
  // DXGI_SWAP_EFFECT_DISCARD, the blt model. In exclusive fullscreen that flips
  // straight to the display, but a windowed blt chain is composited: every
  // present copies the backbuffer into the compositor's surface, and a frame
  // that misses vblank waits for the next one, so the rate halves rather than
  // degrading. A player who picks Windowed is on that path.
  //
  // FLIP_DISCARD is the fix for it, but do not set it here without settling one
  // thing first. A flip-model backbuffer is unbound after every present, so
  // something has to bind it each frame. The supersampling pass only rebinds it
  // when a real render/display difference arms it, so what decides the question
  // is whether these games rebind their own backbuffer view every frame, and
  // nothing here establishes that.
  //
  // Seed the main render size from the configuration rather than waiting to
  // recognise it from a texture. It is known here -- it is what the ini says --
  // and until it is set, the auxiliary-target branch in CreateTexture2D is
  // gated off, so anything created before the main target happens to appear is
  // classified against nothing and silently left at its original size. The
  // detection below still runs and still logs; this only removes the window
  // where the answer was available but not yet recorded.
  {
    UINT seedWidth = 0;
    UINT seedHeight = 0;
    if (renderResolution(&seedWidth, &seedHeight) &&
        !g_mainRtWidth.load(std::memory_order_relaxed)) {
      g_mainRtWidth.store(seedWidth, std::memory_order_relaxed);
      g_mainRtHeight.store(seedHeight, std::memory_order_relaxed);
      if (verboseLogging())
        log("Main render size seeded from the configuration: ", std::dec,
            seedWidth, "x", seedHeight);
    }
  }

  pDesc->BufferDesc.Width = width;
  pDesc->BufferDesc.Height = height;
  pDesc->BufferDesc.RefreshRate.Numerator = 0;
  pDesc->BufferDesc.RefreshRate.Denominator = 0;
  static std::atomic<uint32_t> reportedResolutionOverride{0};
  if (logFirstOrVerbose(reportedResolutionOverride))
    log("Overriding swap-chain resolution to ", std::dec, width, "x", height);

  // IN A WINDOW THE BACKBUFFER DOES NOT DECIDE THE SHAPE ON SCREEN. The window
  // does, and the game sized it from its own [Graphics] ScreenWidth/ScreenHeight
  // before this hook ran. A DXGI_SWAP_EFFECT_DISCARD chain stretches the
  // backbuffer into whatever client area it finds, so correcting the backbuffer
  // alone leaves the same stretched picture, produced one step later.
  //
  // pDesc->Windowed rather than the ini: a fact about the chain being created,
  // not a setting somebody may have edited since.
  if (pDesc->Windowed && pDesc->OutputWindow)
    resizeClientArea(pDesc->OutputWindow, width, height);
  return true;
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
std::atomic<UINT> g_largestViewportWidth{0};
std::atomic<UINT> g_largestViewportHeight{0};

void noteViewportExtent(UINT width, UINT height) {
  UINT seen = g_largestViewportWidth.load(std::memory_order_relaxed);
  while (width > seen && !g_largestViewportWidth.compare_exchange_weak(
      seen, width, std::memory_order_relaxed)) {}
  seen = g_largestViewportHeight.load(std::memory_order_relaxed);
  while (height > seen && !g_largestViewportHeight.compare_exchange_weak(
      seen, height, std::memory_order_relaxed)) {}
}

// ---- scene-target identity -------------------------------------------------
// The one surface the games composite the finished frame into. supersample.h
// states the architectural fact this rests on: these games bind the swap-chain
// backbuffer itself as their colour render target. Under supersampling, every
// RTV over that backbuffer is redirected to the mod's render-resolution
// texture, so that texture is the composite target instead.
//
// SMAA has to identify this surface to find the scene/UI boundary, and it used
// to do so by dimensions. That cannot work in this mod: CreateTexture2D
// deliberately promotes the engine's hard-coded 1920x1080 auxiliary targets to
// the main render size, so blur and snapshot targets become byte-identical to
// the scene target under any size-based test. Anchoring on identity removes the
// ambiguity entirely rather than trying to score it.
std::atomic<void*> g_sceneAnchor{nullptr};
// An AddRef'd handle to the same texture. The command-list injection point runs
// outside any bind, so it needs the resource itself rather than a bare identity.
std::atomic<ID3D11Texture2D*> g_sceneAnchorTex{nullptr};
// Rorona records its whole frame on the deferred context, in more than one
// command list, and the immediate context executes them in the order they were
// finished. Passes recorded onto the deferred context never reach the presented
// frame, so they are issued on the immediate context instead -- immediately
// after the list that drew the scene, which is exactly the pre-UI point.
//
// Pairing matters: the scene/UI boundary is observed at the very end of
// recording, so keying off it lands after the UI list. What identifies the
// right list is whether the scene itself was drawn in it.

// The scene target identified by what is drawn into it, not by its size or by
// assuming it is the backbuffer. Rorona renders the 3D scene into an offscreen
// target, post-processes it, draws the UI into separate targets, and composites
// everything into the backbuffer at the end -- so the backbuffer only ever
// holds the finished, UI-included frame.
//
// Dimensions cannot pick the scene target out: the mod promotes the engine's
// hard-coded 1920x1080 auxiliary targets to the main render size, so several
// textures match it exactly. Depth-tested draw count can: only the 3D pass
// accumulates hundreds of them. SMAA runs when the game binds away from that
// target, which is the end of the 3D pass and before any UI exists.
std::atomic<void*> g_sceneRt{nullptr};
std::atomic<ID3D11Texture2D*> g_sceneRtTex{nullptr};
std::atomic<uint32_t> g_sceneRtDraws{0};

// Serialises the tracked-texture swap in noteSceneRtDraw against the
// load-and-AddRef in fireSceneRtSmaa. Without it a second recording thread can
// replace and release the texture in the window between the other thread's load
// and its AddRef, and the per-frame reset does not drop this reference, so that
// swap is the only place it can go. Off the hot path: taken when the tracked
// target changes, and once per frame when the passes fire.
mutex g_sceneRtMutex;

// Enough draws to be the 3D pass rather than a stray depth-tested blit.
constexpr uint32_t kSceneDrawThreshold = 24;

// The SMAA passes bind render targets themselves, re-entering the very hook
// that triggers them. Without this the injector recurses until the stack is
// gone -- the counter it self-limits on is only cleared once the passes return.
// Thread-local because the recursion is always on the recording thread, and a
// global flag would let one thread suppress another's legitimate injection.
// Safe against the context migration below because a pass runs start to finish
// inside one call, so the thread cannot change underneath the guard.
thread_local bool t_inSmaaPasses = false;

struct SmaaReentryGuard {
  bool entered;
  SmaaReentryGuard() : entered(!t_inSmaaPasses) { t_inSmaaPasses = true; }
  ~SmaaReentryGuard() { if (entered) t_inSmaaPasses = false; }
};
// Per-list composition, to establish whether the scene and the UI are recorded
// into the same command list. If they are, there is no boundary between lists
// and the injection point has to be inside the recorded stream instead.

// Re-evaluated every frame rather than latched once. The game changes display
// mode after creating the swap chain, and ResizeBuffers destroys and recreates
// the backbuffer textures -- a pointer captured at creation would silently
// refer to freed memory and never match again. Refreshing costs one GetBuffer
// per present and is immune to resizes, mode changes and buffer rotation.
void noteSceneAnchor(IDXGISwapChain* swapChain) {
  if (!swapChain)
    return;
  void* found = nullptr;
  const char* which = nullptr;
  // Supersampling: the render-resolution stand-in is what the game actually
  // renders into. Ask first, because in that case the backbuffer is touched
  // only by the downscale.
  if (ID3D11Texture2D* ssaa = atfix::ssaaAcquireColor()) {
    found = ssaa;
    which = "render-resolution texture";
    ssaa->Release();   // identity only; the module owns the reference
  } else {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
      found = back;
      which = "swap-chain backbuffer";
      back->Release();   // identity only; the swap chain owns the reference
    }
  }
  if (!found)
    return;
  if (g_sceneAnchor.exchange(found, std::memory_order_relaxed) != found) {
    // The reference is the point, not the pointer: the anchor is compared by
    // ADDRESS below, so holding one stops a freed texture's address being
    // recycled and faking a match. It also keeps a reference outstanding on
    // whatever it anchored, which would make ResizeBuffers fail. A probe on the
    // swap chain's ResizeBuffers saw no call in any of the three games, though
    // all three do contain one. supersample.cpp records where that call is and
    // what the measurement does and does not establish; the same reasoning
    // covers it, because it holds a view over the real backbuffer for the
    // process lifetime whenever supersampling is on.
    ID3D11Texture2D* tex = static_cast<ID3D11Texture2D*>(found);
    tex->AddRef();
    if (ID3D11Texture2D* prev =
          g_sceneAnchorTex.exchange(tex, std::memory_order_relaxed))
      prev->Release();
    if (verboseLogging())
      log("SMAA scene target anchored to the ", which, " ", found);
  }
}

// Totori's scene target test, unchanged from the implementation validated for
// it: a main-size single-sample colour view. Rorona and Meruru cannot use this
// -- the mod promotes their hard-coded auxiliary targets to the same size, so
// several textures match -- but Totori draws its UI onto the target this finds,
// which is what makes its depth-state boundary genuinely pre-UI.
bool smaaMainSizeColor(ID3D11RenderTargetView* rtv, ID3D11Texture2D** outTex) {
  if (outTex) *outTex = nullptr;
  if (!rtv)
    return false;
  ID3D11Resource* res = nullptr;
  rtv->GetResource(&res);
  bool match = false;
  if (res) {
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
      D3D11_TEXTURE2D_DESC d = {};
      tex->GetDesc(&d);
      const UINT w = g_mainRtWidth.load(std::memory_order_relaxed);
      const UINT h = g_mainRtHeight.load(std::memory_order_relaxed);
      match = w && d.Width == w && d.Height == h && d.SampleDesc.Count == 1;
      if (match && outTex) { *outTex = tex; tex = nullptr; }
      if (tex) tex->Release();
    }
    res->Release();
  }
  return match;
}

// Is this view's resource the composite target? Used by the Arland pair only.
bool smaaIsSceneTarget(ID3D11RenderTargetView* rtv, ID3D11Texture2D** outTex) {
  if (outTex) *outTex = nullptr;
  void* anchor = g_sceneAnchor.load(std::memory_order_relaxed);
  if (!rtv || !anchor)
    return false;
  ID3D11Resource* res = nullptr;
  rtv->GetResource(&res);
  bool match = false;
  if (res) {
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)))) {
      match = static_cast<void*>(tex) == anchor;
      if (match && outTex) { *outTex = tex; tex = nullptr; }
      if (tex) tex->Release();
    }
    res->Release();
  }
  return match;
}

// Main-size single-sample test for a resource rather than a view.
bool smaaMainSizeHostResource(ID3D11Resource* res) {
  ID3D11Texture2D* tex = nullptr;
  if (!res || FAILED(res->QueryInterface(IID_PPV_ARGS(&tex))) || !tex)
    return false;
  D3D11_TEXTURE2D_DESC d = {};
  tex->GetDesc(&d);
  const UINT w = g_mainRtWidth.load(std::memory_order_relaxed);
  const UINT h = g_mainRtHeight.load(std::memory_order_relaxed);
  const bool match =
    w && d.Width == w && d.Height == h && d.SampleDesc.Count == 1;
  tex->Release();
  return match;
}

bool smaaIsSceneTargetResource(ID3D11Resource* res) {
  void* anchor = g_sceneAnchor.load(std::memory_order_relaxed);
  if (!res || !anchor)
    return false;
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(res->QueryInterface(IID_PPV_ARGS(&tex))) || !tex)
    return false;
  const bool match = static_cast<void*>(tex) == anchor;
  tex->Release();
  return match;
}

// ---- pre-UI SMAA injection -------------------------------------------------
// Run SMAA on the finished 3D scene, before the UI composites on top of it,
// matching AGT's injection point, so the HUD and menus stay crisp. Which draw
// marks that boundary differs per title and is chosen in smaaBoundaryMode below.
// Once per frame; the latch is reset at Present.
std::atomic<bool> g_smaaDoneThisFrame{false};
std::atomic<bool> g_smaaSceneSeen{false};

struct SmaaTrackedState {
  std::atomic<bool> mainDepthBound{false};
  std::atomic<bool> depthDisabled{false};
};

SmaaTrackedState g_smaaImmediate;
SmaaTrackedState g_smaaDeferred;

SmaaTrackedState* smaaTrackedState(
    ID3D11DeviceContext* context) {
  return context == g_immCtx.load(std::memory_order_relaxed)
    ? &g_smaaImmediate : &g_smaaDeferred;
}

// Which scene/UI boundary to use. Rorona and Meruru use the scene-target
// boundary (SceneRt); Totori has no usable render-target signal there and uses
// the depth-state one. The reasoning for each is beside the return below.
// The boundaries share the once-per-frame latch, so running two lets whichever
// fires first suppress the other -- which is why this is a choice, not a union.
// ARLAND_SMAA_BOUNDARY=target|depth|both|scene overrides.
enum class SmaaBoundary { TargetBind, DepthState, Both, SceneRt };

SmaaBoundary smaaBoundaryMode() {
  static const SmaaBoundary mode = [] {
    if (const char* v = std::getenv("ARLAND_SMAA_BOUNDARY")) {
      if (v[0] == 'd') return SmaaBoundary::DepthState;
      if (v[0] == 'b') return SmaaBoundary::Both;
      if (v[0] == 't') return SmaaBoundary::TargetBind;
      if (v[0] == 's') return SmaaBoundary::SceneRt;
    }
    // Rorona and Meruru composite scene and UI into the backbuffer only at the
    // very end of the frame, so every render-target or depth-state boundary
    // reachable there is already post-UI. Their pre-UI point is the moment the
    // 3D pass stops being the bound target, which is what SceneRt keys off.
    // Totori draws its UI onto the scene target itself, so its depth-state
    // boundary is genuinely pre-UI and stays.
    return currentTitle() == Title::Totori ? SmaaBoundary::DepthState
                                           : SmaaBoundary::SceneRt;
  }();
  return mode;
}

// Who needs the pre-UI boundary detected at all. Two passes are placed there,
// edge smoothing and sharpening, and sharpen.h records that they are separate
// settings: sharpening a frame that was never antialiased is what the Sharpen
// slider means with SMAA off. Detecting the boundary only when SMAA was on
// therefore left that slider doing nothing, silently, because every call site
// of sharpenApply sits behind one of the four gates below.
//
// Opening the boundary for sharpening alone cannot run SMAA: smaaApplySceneColor
// gates on smaaEnabled() itself and returns false.
bool preUiBoundaryNeeded() {
  static const bool needed =
    (atfix::smaaEnabled() || atfix::sharpenEnabled()) && atfix::smaaPreUI();
  return needed;
}

bool smaaTargetBindBoundaryEnabled() {
  const SmaaBoundary mode = smaaBoundaryMode();
  return mode == SmaaBoundary::TargetBind || mode == SmaaBoundary::Both;
}

bool smaaDepthStateBoundaryEnabled() {
  const SmaaBoundary mode = smaaBoundaryMode();
  return mode == SmaaBoundary::DepthState || mode == SmaaBoundary::Both;
}

bool smaaSceneRtBoundaryEnabled() {
  // The other two boundaries are ANDed with the feature where they are read, in
  // trackSmaaRenderTargets and smaaDrawBoundary. This one is read straight from
  // the draw and bind paths, so it carries the gate itself: without it a
  // boundary nothing consumes still pays four state queries per draw and keeps
  // a reference on the scene target that nothing reads.
  return preUiBoundaryNeeded() && smaaBoundaryMode() == SmaaBoundary::SceneRt;
}

// Temporary boundary-sequence trace; defined below, used by the setter hooks.
void fireSceneRtSmaa(ID3D11DeviceContext* context, const char* reason);

// Setter hooks maintain the two facts the depth-state boundary needs, so the
// hot draw path is an atomic-flag read rather than a pair of D3D11 state
// queries. depthDisabled also drives the scene-target injector below.
void trackSmaaRenderTargets(
    ID3D11DeviceContext* context, UINT rtvCount,
    ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
  if (!preUiBoundaryNeeded())
    return;
  const bool mainWithDepth = rtvCount >= 1 && rtvs && rtvs[0] && dsv &&
    smaaMainSizeColor(rtvs[0], nullptr);
  smaaTrackedState(context)->mainDepthBound.store(
    mainWithDepth, std::memory_order_relaxed);
}

void trackSmaaDepthState(
    ID3D11DeviceContext* context, ID3D11DepthStencilState* state) {
  D3D11_DEPTH_STENCIL_DESC desc = {};
  desc.DepthEnable = TRUE;   // null means D3D11's depth-enabled default
  if (state)
    state->GetDesc(&desc);
  smaaTrackedState(context)->depthDisabled.store(
    !desc.DepthEnable, std::memory_order_relaxed);
}

void smaaFireOnSceneRtRelease(ID3D11DeviceContext* context,
                              ID3D11RenderTargetView* const* rtvs,
                              UINT rtvCount) {
  if (!smaaSceneRtBoundaryEnabled() || t_inSmaaPasses)
    return;
  void* scene = g_sceneRt.load(std::memory_order_relaxed);
  if (!scene)
    return;
  // Still bound? The 3D pass is not finished.
  if (rtvCount >= 1 && rtvs && rtvs[0]) {
    ID3D11Resource* res = nullptr;
    rtvs[0]->GetResource(&res);
    const bool same = static_cast<void*>(res) == scene;
    if (res) res->Release();
    if (same)
      return;
  }
  // Backstop: only reached when the UI never drew onto the scene target, so
  // the depth-off trigger above never fired.
  fireSceneRtSmaa(context, "bind_away");
}

// Issue the passes over the tracked scene target. The callers decide *when*;
// this decides whether there is anything to do, and does it once.
void fireSceneRtSmaa(ID3D11DeviceContext* context, const char* reason) {
  const uint32_t draws = g_sceneRtDraws.load(std::memory_order_relaxed);
  if (draws < kSceneDrawThreshold)
    return;
  // Load and AddRef under the lock: the swap in noteSceneRtDraw can release the
  // texture between the two, and the reference it drops is the only one the mod
  // holds.
  ID3D11Texture2D* tex = nullptr;
  {
    std::lock_guard lock(g_sceneRtMutex);
    tex = g_sceneRtTex.load(std::memory_order_relaxed);
    if (tex)
      tex->AddRef();
  }
  if (!tex)
    return;
  // Cleared before running, not after: the passes re-enter the hooks below.
  g_sceneRtDraws.store(0, std::memory_order_relaxed);
  SmaaReentryGuard guard;
  const bool ran = atfix::smaaApplySceneColor(context, tex);
  // Sharpening rides every pre-UI boundary SMAA does, on the same surface and
  // always after it: sharpening first would only give SMAA harder edges to
  // blend away again. It does not depend on that pass having run. With SMAA off
  // this is a sharpening filter on the finished scene, still pre-UI, which is
  // why preUiBoundaryNeeded() opens the boundary for either setting.
  const bool sharpened = atfix::sharpenApply(context, tex);
  static std::atomic<bool> logged{false};
  if (verboseLogging() && !logged.exchange(true, std::memory_order_relaxed))
    log("PRE-UI: injection on the scene target, reason=", reason,
        " depth_draws=", std::dec, draws, " smaa=", ran ? 1 : 0,
        " sharpen=", sharpened ? 1 : 0);
  tex->Release();
}

// Every draw. Depth-tested draws into a main-size single-sample colour target
// identify the 3D pass -- dimensions alone cannot, because the mod promotes the
// engine's hard-coded 1920x1080 auxiliary targets to the main render size, but
// only the 3D pass accumulates hundreds of depth-tested draws.
//
// The first draw into that same target with depth testing OFF is the first UI
// draw: these games composite the HUD straight onto the finished scene. That is
// the pre-UI moment, and it has to be taken before the draw rather than after.
void noteSceneRtDraw(ID3D11DeviceContext* context) {
  if (!smaaSceneRtBoundaryEnabled() || t_inSmaaPasses)
    return;
  SmaaTrackedState* tracked = smaaTrackedState(context);
  const bool depthOff = tracked->depthDisabled.load(std::memory_order_relaxed);

  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  context->OMGetRenderTargets(1, &rtv, &dsv);
  ID3D11Resource* res = nullptr;
  if (rtv) rtv->GetResource(&res);
  ID3D11Texture2D* tex = nullptr;
  if (res) res->QueryInterface(IID_PPV_ARGS(&tex));

  if (tex) {
    if (depthOff) {
      if (static_cast<void*>(tex) == g_sceneRt.load(std::memory_order_relaxed))
        fireSceneRtSmaa(context, "first_ui_draw");
    } else if (dsv) {
      D3D11_TEXTURE2D_DESC d = {};
      tex->GetDesc(&d);
      const UINT w = g_mainRtWidth.load(std::memory_order_relaxed);
      const UINT h = g_mainRtHeight.load(std::memory_order_relaxed);
      if (w && d.Width == w && d.Height == h && d.SampleDesc.Count == 1) {
        if (g_sceneRt.exchange(tex, std::memory_order_relaxed) != tex) {
          g_sceneRtDraws.store(0, std::memory_order_relaxed);
          ID3D11Texture2D* previous = nullptr;
          {
            std::lock_guard lock(g_sceneRtMutex);
            tex->AddRef();
            previous = g_sceneRtTex.exchange(tex, std::memory_order_relaxed);
          }
          // Released outside the lock. The lock exists so this swap cannot
          // interleave with fireSceneRtSmaa's load-and-AddRef, which does not
          // depend on when the old reference is dropped. Dropping the last one
          // destroys the texture, and that runs runtime code under whatever
          // locks it wants; keeping it out here means our lock is never held
          // across anything that can do real work. AddRef stays inside, being
          // an interlocked increment that cannot call back.
          if (previous)
            previous->Release();
        }
        g_sceneRtDraws.fetch_add(1, std::memory_order_relaxed);
      }
    }
    tex->Release();
  }
  if (res) res->Release();
  if (rtv) rtv->Release();
  if (dsv) dsv->Release();
}

void smaaDrawBoundary(ID3D11DeviceContext* context) {
  // Which thread each context records draws on. Keyed by context and reported
  // only when that context's thread changes, so a context that stays put costs
  // one line and one that moves says so on the frame it moves. Three things it
  // separates: whether a second deferred recorder exists, whether a context
  // migrates between threads, and whether the thread_local guards in this file
  // partition the way the code assumes. All four draw hooks reach here, and
  // this runs before any per-title gating, so every build reports.
  if (verboseLogging()) {
    static mutex threadMutex;
    static std::map<ID3D11DeviceContext*, DWORD> contextThreads;
    const DWORD thread = GetCurrentThreadId();
    std::lock_guard lock(threadMutex);
    DWORD& seen = contextThreads[context];
    if (seen != thread) {
      log("CTX_THREAD context=", context,
          " kind=", context == g_immCtx.load(std::memory_order_relaxed)
                      ? "immediate" : "deferred",
          " thread=", std::dec, thread, " previous=", seen);
      seen = thread;
    }
  }
  noteSceneRtDraw(context);
  static const bool enabled =
    preUiBoundaryNeeded() && smaaDepthStateBoundaryEnabled();
  if (!enabled ||
      g_smaaDoneThisFrame.load(std::memory_order_relaxed))
    return;
  SmaaTrackedState* tracked = smaaTrackedState(context);
  const bool depthBound = tracked->mainDepthBound.load(std::memory_order_relaxed);
  const bool depthOff = tracked->depthDisabled.load(std::memory_order_relaxed);
  const bool sceneSeen = g_smaaSceneSeen.load(std::memory_order_relaxed);
  // Which of the three conditions is missing is the whole diagnosis when the
  // boundary never fires, so report each distinct combination once.
  if (!depthBound || !depthOff || !sceneSeen)
    return;

  ID3D11RenderTargetView* rtv = nullptr;
  context->OMGetRenderTargets(1, &rtv, nullptr);
  if (!rtv)
    return;

  ID3D11Texture2D* scene = nullptr;
  smaaMainSizeColor(rtv, &scene);

  if (scene) {
    g_smaaDoneThisFrame.store(true, std::memory_order_relaxed);
    const bool ran = atfix::smaaApplySceneColor(context, scene);
    const bool sharpened = atfix::sharpenApply(context, scene);
    static std::atomic<bool> depthBoundaryLogged{false};
    if (verboseLogging() &&
        !depthBoundaryLogged.exchange(true, std::memory_order_relaxed))
      log("PRE-UI: depth-state boundary active smaa=", ran ? 1 : 0,
          " sharpen=", sharpened ? 1 : 0);
    scene->Release();
  }
  rtv->Release();
}

// ---- present-path probe (ARLAND_PRESENT_TRACE) ----------------------------
// One-shot diagnostic that answers how the finished frame reaches the swap-
// chain backbuffer, which decides where supersampling inserts its downscale.
// It reports the backbuffer resource and size at Present, whether the main-size
// colour target the scene/UI composite into IS that backbuffer (Scenario A: the
// game renders straight into the backbuffer, no transfer) or a separate texture
// (Scenario B: something copies it in), and any copy whose destination is the
// backbuffer. Needs a resolution override active so g_mainRt is populated. Gated
// so normal builds are unaffected. Backbuffer pointers are only compared for
// identity, never dereferenced, so holding a released COM pointer value is safe.
bool presentTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_PRESENT_TRACE");
    return value && value[0] != '0';
  }();
  return enabled;
}

void largestViewportSeen(unsigned int* width, unsigned int* height) {
  *width = g_largestViewportWidth.load(std::memory_order_relaxed);
  *height = g_largestViewportHeight.load(std::memory_order_relaxed);
}

std::atomic<void*> g_traceBackbuffer{nullptr};

// One-pixel GPU readback from the centre of a single-sample texture, through
// original entry points so it stays invisible to the hooks and the trace.
// Diagnostic-only: the copy-then-map is a pipeline sync on every call, which
// is acceptable in a trace run and never happens otherwise. The 1x1 staging
// texture is cached per format and intentionally never released.
// Per-Present content marker for the trace. The wiring log proved the
// conversation backdrop is captured by a fullscreen draw FROM the swap-chain
// backbuffer, whose only writer under supersampling is the mod's own blit. The
// wiring being right leaves the CONTENT the backbuffer holds when the capture
// executes, which no event line can carry. Called at the top of the Present
// hook, before the blit, so back_px is what this frame's deferred
// execution could still have sampled; host_px separates "the stand-in went
// black" from "the blit stopped carrying it".
void notePresentBackbuffer(IDXGISwapChain* swapChain) {
  if (!presentTraceEnabled() || !swapChain)
    return;
  ID3D11Texture2D* backbuffer = nullptr;
  if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))) || !backbuffer)
    return;
  void* previous = g_traceBackbuffer.exchange(
    backbuffer, std::memory_order_relaxed);
  if (previous != static_cast<void*>(backbuffer)) {
    D3D11_TEXTURE2D_DESC d = { };
    backbuffer->GetDesc(&d);
    log("PRESENTTRACE backbuffer=", static_cast<void*>(backbuffer),
        " size=", std::dec, d.Width, "x", d.Height,
        " format=", d.Format,
        " mainRt=", g_mainRtWidth.load(std::memory_order_relaxed), "x",
        g_mainRtHeight.load(std::memory_order_relaxed));
  }
  backbuffer->Release();
}

bool isTraceBackbuffer(ID3D11Resource* resource) {
  if (!presentTraceEnabled() || !resource)
    return false;
  void* backbuffer = g_traceBackbuffer.load(std::memory_order_relaxed);
  if (!backbuffer)
    return false;
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&tex))) || !tex)
    return false;
  const bool match = static_cast<void*>(tex) == backbuffer;
  tex->Release();
  return match;
}

// Called from OMSetRenderTargets with the incoming binding. The depth-less
// main-size colour bind is where the finished scene lives just before the UI
// draws; comparing that target to the backbuffer is the decisive A-vs-B signal.
void presentTraceRenderTargets(UINT rtvCount,
                               ID3D11RenderTargetView* const* rtvs,
                               ID3D11DepthStencilView* dsv) {
  if (!presentTraceEnabled() || !rtvs || rtvCount == 0)
    return;
  const void* backbuffer = g_traceBackbuffer.load(std::memory_order_relaxed);
  if (!backbuffer)
    return;   // backbuffer is not captured until the first Present
  const UINT mw = g_mainRtWidth.load(std::memory_order_relaxed);
  const UINT mh = g_mainRtHeight.load(std::memory_order_relaxed);
  static std::atomic<uint32_t> logged{0};
  // Log any bound render target that IS the backbuffer or is main-sized, with or
  // without depth. If a main-size colour RTV equals the backbuffer, the game
  // renders straight into it (Scenario A); a main-size RTV that is NOT the
  // backbuffer means a separate main target that must be copied in (Scenario B),
  // which the copy-into-backbuffer probe then catches.
  for (UINT i = 0; i < rtvCount && i < 8; ++i) {
    if (!rtvs[i])
      continue;
    ID3D11Resource* res = nullptr;
    rtvs[i]->GetResource(&res);
    if (!res)
      continue;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex))) && tex) {
      D3D11_TEXTURE2D_DESC d = { };
      tex->GetDesc(&d);
      const bool isBack = static_cast<void*>(tex) == backbuffer;
      const bool mainSize = mw && d.Width == mw && d.Height == mh;
      if ((isBack || mainSize) &&
          logged.fetch_add(1, std::memory_order_relaxed) < 8) {
        log("PRESENTTRACE rtv res=", static_cast<void*>(tex), " ", std::dec,
            d.Width, "x", d.Height, " samples=", d.SampleDesc.Count,
            " depth=", dsv ? "yes" : "no", " is_backbuffer=",
            isBack ? "YES (scenario A)" : "no (separate RT)");
      }
      tex->Release();
    }
    res->Release();
  }
}

// Called at the top of OMSetRenderTargets with the INCOMING (pre-substitution)
// binding. Detects the scene→UI boundary and runs pre-UI SMAA once per frame.
void smaaSceneBoundary(ID3D11DeviceContext* context, UINT rtvCount,
                       ID3D11RenderTargetView* const* rtvs,
                       ID3D11DepthStencilView* dsv) {
  if (!preUiBoundaryNeeded() || rtvCount != 1 || !rtvs || !rtvs[0])
    return;
  if (dsv) {
    // Main colour + depth = the scene pass. Feeds Totori's depth-state
    // boundary; the Arland pair's injector uses its own draw counting.
    if (smaaMainSizeColor(rtvs[0], nullptr))
      g_smaaSceneSeen.store(true, std::memory_order_relaxed);
    return;
  }
  if (!smaaTargetBindBoundaryEnabled())
    return;
  // Composite target without depth = UI start. SMAA the finished scene once.
  if (!g_smaaSceneSeen.load(std::memory_order_relaxed) ||
      g_smaaDoneThisFrame.load(std::memory_order_relaxed))
    return;
  ID3D11Texture2D* scene = nullptr;
  if (smaaIsSceneTarget(rtvs[0], &scene) && scene) {
    g_smaaDoneThisFrame.store(true, std::memory_order_relaxed);
    // The scene latch has been spent; clear it so nothing re-enters before
    // Present resets the frame.
    g_smaaSceneSeen.store(false, std::memory_order_relaxed);
    const bool ran = atfix::smaaApplySceneColor(context, scene);
    const bool sharpened = atfix::sharpenApply(context, scene);
    static std::atomic<bool> logged{false};
    if (verboseLogging() && !logged.exchange(true, std::memory_order_relaxed))
      log("PRE-UI: target-bind boundary active smaa=", ran ? 1 : 0,
          " sharpen=", sharpened ? 1 : 0);
    scene->Release();
  }
}

void smaaResetFrame() {
  g_smaaDoneThisFrame.store(false, std::memory_order_relaxed);
  g_smaaSceneSeen.store(false, std::memory_order_relaxed);
  g_sceneRt.store(nullptr, std::memory_order_relaxed);
  g_sceneRtDraws.store(0, std::memory_order_relaxed);
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

  static const std::array<FormatRange, 12> s_ranges = {{
    { DXGI_FORMAT_R32G32B32A32_TYPELESS,  DXGI_FORMAT_R32G32B32A32_SINT,       16u },
    { DXGI_FORMAT_R32G32B32_TYPELESS,     DXGI_FORMAT_R32G32B32_SINT,          12u },
    { DXGI_FORMAT_R16G16B16A16_TYPELESS,  DXGI_FORMAT_R32G32_SINT,             8u  },
    { DXGI_FORMAT_R32G8X24_TYPELESS,      DXGI_FORMAT_X32_TYPELESS_G8X24_UINT, 8u  },
    { DXGI_FORMAT_R10G10B10A2_TYPELESS,   DXGI_FORMAT_R32_SINT,                4u  },
    { DXGI_FORMAT_R24G8_TYPELESS,         DXGI_FORMAT_X24_TYPELESS_G8_UINT,    4u  },
    { DXGI_FORMAT_R9G9B9E5_SHAREDEXP,     DXGI_FORMAT_R9G9B9E5_SHAREDEXP,      4u  },
    { DXGI_FORMAT_B8G8R8A8_UNORM,         DXGI_FORMAT_B8G8R8X8_UNORM_SRGB,     4u  },
    { DXGI_FORMAT_R8G8_TYPELESS,          DXGI_FORMAT_R16_SINT,                2u  },
    { DXGI_FORMAT_B5G6R5_UNORM,           DXGI_FORMAT_B5G5R5A1_UNORM,          2u  },
    { DXGI_FORMAT_B4G4R4A4_UNORM,         DXGI_FORMAT_B4G4R4A4_UNORM,          2u  },
    { DXGI_FORMAT_R8_TYPELESS,            DXGI_FORMAT_A8_UNORM,                1u  },
  }};

  // Buffers report DXGI_FORMAT_UNKNOWN and measure their box in bytes rather
  // than in texels, so one byte per element is the correct answer here, not a
  // miss. This is by far the most common call: left in the miss path below it
  // logged on every buffer copy, over 300k identical lines in one session,
  // rotating every useful diagnostic out of the file.
  if (Format == DXGI_FORMAT_UNKNOWN)
    return 1u;

  for (const auto& range : s_ranges) {
    if (Format >= range.MinFormat && Format <= range.MaxFormat)
      return range.FormatSize;
  }

  // A genuine miss, worth reporting, but once per format: the callers are copy
  // and byte-estimate paths that run per resource per frame. Block-compressed
  // formats land here and have no per-texel size at all: BC1 is half a byte
  // per texel, BC2/BC3 one byte, and their rows are counted in 4x4 blocks.
  // Zero is a refusal rather than a wrong answer. The copy paths stand down on
  // it and take the real GPU copy; the byte estimators count nothing.
  // Static storage zero-initializes, so every slot starts false. A value past
  // the end of the table has no slot to mark and so needs its own flag:
  // without one it reports on every call, which is the flood the per-slot flags
  // exist to prevent.
  static std::array<std::atomic<bool>, 256> s_logged;
  static std::atomic<bool> s_loggedOutOfRange;
  const size_t slot = size_t(Format);
  const bool first = slot < s_logged.size()
    ? !s_logged[slot].exchange(true, std::memory_order_relaxed)
    : !s_loggedOutOfRange.exchange(true, std::memory_order_relaxed);
  if (first)
    log("Unhandled format ", Format);

  return 0u;
}

// Fills pInfo from the resource, false if it cannot. The interface query in
// each branch is checked: a resource that answered GetType is expected to
// answer the matching QueryInterface, but a failure would hand the branch a
// null interface and fault on GetDesc. Returning false instead is the answer
// the callers already handle, and it keeps the failure inside the layer whose
// job is to absorb it -- tryCpuCopy takes the real GPU copy on a false here.
bool getResourceInfo(
        ID3D11Resource*           pResource,
        ATFIX_RESOURCE_INFO*      pInfo) {
  pResource->GetType(&pInfo->Dim);

  switch (pInfo->Dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
      ID3D11Buffer* buffer = nullptr;
      if (FAILED(pResource->QueryInterface(IID_PPV_ARGS(&buffer))) || !buffer)
        return false;

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
      if (FAILED(pResource->QueryInterface(IID_PPV_ARGS(&texture))) || !texture)
        return false;

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
      if (FAILED(pResource->QueryInterface(IID_PPV_ARGS(&texture))) || !texture)
        return false;

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
      if (FAILED(pResource->QueryInterface(IID_PPV_ARGS(&texture))) || !texture)
        return false;

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
      static std::atomic<uint32_t> reported{0};
      if (logFirstOrVerbose(reported))
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
  // Mips is 0 only when getResourceInfo failed and the caller did not check.
  // A divide by zero is not a graceful way to find that out.
  uint32_t mip = pInfo->Mips ? Subresource % pInfo->Mips : 0;

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
      // Same reason as getResourceInfo: an unanswered query would fault on
      // GetDesc, and a failed HRESULT here is the path the caller handles.
      ID3D11Buffer* buffer = nullptr;
      if (FAILED(pBaseResource->QueryInterface(IID_PPV_ARGS(&buffer))) ||
          !buffer) {
        hr = E_FAIL;
        break;
      }

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
      if (FAILED(pBaseResource->QueryInterface(IID_PPV_ARGS(&texture))) ||
          !texture) {
        hr = E_FAIL;
        break;
      }

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
      if (FAILED(pBaseResource->QueryInterface(IID_PPV_ARGS(&texture))) ||
          !texture) {
        hr = E_FAIL;
        break;
      }

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
      if (FAILED(pBaseResource->QueryInterface(IID_PPV_ARGS(&texture))) ||
          !texture) {
        hr = E_FAIL;
        break;
      }

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
      hr = E_INVALIDARG;
  }

  if (SUCCEEDED(hr)) {
    procs->CopyResource(pContext, shadowResource, pBaseResource);
    pBaseResource->SetPrivateDataInterface(IID_StagingShadowResource, shadowResource);
  } else {
    static std::atomic<uint32_t> reported{0};
    if (logFirstOrVerbose(reported))
      log("Failed to create shadow resource, dimension=", std::dec,
          resourceInfo.Dim, " hr=0x", std::hex, hr);
  }

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

// ARLAND_ATLAS_RECONCILE: counts font-atlas writes as D3D11 sees them, so the
// menu layer can check them against the middleware unlocks its snapshot
// invalidation actually observes. A D3D11 write with no corresponding unlock is
// a mutation the atlas cache cannot know about, which is the one hazard a
// longer snapshot lifetime would extend. Off by default: the predicate inspects
// every mapped resource.
std::atomic<uint64_t> g_atlasWriteMaps = { 0 };

bool atlasReconcileEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_ATLAS_RECONCILE");
    return value && value[0] != '0';
  }();
  return enabled;
}

uint64_t atlasWriteMapCount() {
  return g_atlasWriteMaps.load(std::memory_order_relaxed);
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

        default: {
          static std::atomic<uint32_t> reported{0};
          if (logFirstOrVerbose(reported))
            log("Unhandled RTV dimension ", desc.ViewDimension);
        }
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

        default: {
          static std::atomic<uint32_t> reported{0};
          if (logFirstOrVerbose(reported))
            log("Unhandled UAV dimension ", desc.ViewDimension);
        }
      }
    } else {
      static std::atomic<uint32_t> reported{0};
      if (logFirstOrVerbose(reported))
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

  // Gate/dim-hold write path: a constant buffer created WITH initial data
  // (transient per-frame cb pattern) bypasses both the Map and
  // UpdateSubresource hooks -- patch the initial payload too. The 880 receiver
  // gate needed this on Rorona; Totori's D3D11 material pattern makes it
  // plausible for its dim-carrying layouts as well.
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
  D3D11_SUBRESOURCE_DATA dimInit;
  static thread_local std::vector<uint8_t> dimInitCopy;
  if (cutinDimHoldEnabled() && arlandInCinematicBattle() &&
      pDesc && pData && pData->pSysMem &&
      (pDesc->BindFlags & D3D11_BIND_CONSTANT_BUFFER) &&
      dimHoldEligibleSize(pDesc->ByteWidth)) {
    dimInitCopy.assign(static_cast<const uint8_t*>(pData->pSysMem),
      static_cast<const uint8_t*>(pData->pSysMem) + pDesc->ByteWidth);
    if (dimHoldPatch(dimInitCopy.data(), pDesc->ByteWidth)) {
      dimInit = *pData;
      dimInit.pSysMem = dimInitCopy.data();
      pData = &dimInit;
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
      if (verboseLogging())
        log("Created targeted dialogue quad companion at ",
            std::dec, mainWidth, "x", mainHeight);
    } else {
      static std::atomic<uint32_t> reported{0};
      if (logFirstOrVerbose(reported))
        log("Failed to create targeted dialogue quad companion at ",
            std::dec, mainWidth, "x", mainHeight);
    }
  }
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
  return procs->CreatePixelShader(pDevice, pShaderBytecode,
    BytecodeLength, pClassLinkage, ppPixelShader);
}

// Sampler creation is deliberately NOT hooked. An earlier version upgraded the
// games' basic point/linear samplers to D3D11_FILTER_ANISOTROPIC. The bound it
// used starts at D3D11_FILTER_MIN_MAG_MIP_POINT, which is enum zero, so every
// point sampler was upgraded too -- and point sampling is correct for a
// colour-grading LUT, a gradient ramp or a dither table, where filtering smears
// what the texture encodes rather than improving it. Nothing here can tell
// those apart from ordinary content by descriptor alone.
HRESULT STDMETHODCALLTYPE ID3D11Device_CreateDeferredContext(
        ID3D11Device*             pDevice,
        UINT                      Flags,
        ID3D11DeviceContext**     ppDeferredContext) {
  auto procs = getDeviceProcs(pDevice);
  HRESULT hr = procs->CreateDeferredContext(pDevice, Flags, ppDeferredContext);

  if (SUCCEEDED(hr) && ppDeferredContext) {
    // The mod is bimodal -- one immediate context, one deferred -- in
    // smaaTrackedState, getRasterState and getContextProcs. If the game makes
    // several, per-context state is being shared between unrelated recorders.
    static std::atomic<uint32_t> deferredCount{0};
    const uint32_t n = deferredCount.fetch_add(1, std::memory_order_relaxed) + 1;
    log("D3D11 deferred context #", std::dec, n, " created ",
        static_cast<void*>(*ppDeferredContext));
    hookContext(*ppDeferredContext);
  }

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
  bool createShadowTwin = false;

  // Declared out here because it is written while deciding the resize and read
  // after the texture exists.
  UINT role = RoleNone;
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
      // 1920x1080 auxiliary targets. Record 1080p too so the main scene can
      // still be identified, but only resize later targets for higher
      // resolutions.
      // This is the RENDER resolution, not the display one: everything the
      // engine draws follows the main target, and supersampling downscales the
      // finished frame into the (smaller) backbuffer at Present. With no render
      // resolution configured the two are the same and this is the vanilla path.
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
        role = RoleMain;
        UINT overrideWidth = 0;
        UINT overrideHeight = 0;
        if (renderResolution(&overrideWidth, &overrideHeight)) {
          desc.Width = overrideWidth;
          desc.Height = overrideHeight;
          changed = true;
        }
        g_mainRtWidth.store(desc.Width, std::memory_order_relaxed);
        g_mainRtHeight.store(desc.Height, std::memory_order_relaxed);
        mainWidth = desc.Width;
        mainHeight = desc.Height;
        if (verboseLogging())
          log("Detected main render size ", std::dec,
              mainWidth, "x", mainHeight);
      }
    } else if (mainWidth > 1920 && mainHeight > 1080 && !pData) {
      // Auxiliary targets follow the main one. Two sizes count as "full": the
      // hard-coded 1080p the renderers ask for regardless of settings, and the
      // size the game itself asked the swap chain for, which under
      // supersampling is smaller than the main target and must be scaled up
      // with it. When no render resolution is configured the game's own size is
      // not recorded at all and only the 1080p rule applies, as before.
      const UINT gameWidth = g_originalSwapWidth.load(std::memory_order_relaxed);
      const UINT gameHeight = g_originalSwapHeight.load(std::memory_order_relaxed);
      const bool gameSized = gameWidth && gameHeight &&
        (gameWidth != mainWidth || gameHeight != mainHeight) &&
        desc.Width == gameWidth && desc.Height == gameHeight;
      const bool gameHalfSized = gameWidth && gameHeight &&
        (gameWidth != mainWidth || gameHeight != mainHeight) &&
        desc.Width == gameWidth / 2 && desc.Height == gameHeight / 2;
      // 1920x1080 is the engine's hard-coded FULL-size target. It is also
      // exactly half of 3840x2160 -- so on a copy whose own resolution is 4K,
      // a full-size target matches the half-size rule too, and the half branch
      // below would allocate it at half the render size. What that looks like
      // in play is a conversation with the scene in the top-left quarter and
      // black everywhere else, because the snapshot copy into it can only crop.
      //
      // The literal wins the tie. The engine hard-codes 960x540 for its blur
      // targets and creates them at that size even when the game runs at 4K
      // (observed in logs from a 4K copy), so gameHalfSized is not what
      // identifies a blur target in practice, and deferring to the literal
      // costs nothing.
      const bool literalFullSize = desc.Width == 1920 && desc.Height == 1080;
      const bool fullSizeTarget =
        (desc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) &&
        (literalFullSize || gameSized);
      const bool halfSizeBlurTarget =
        (desc.BindFlags & D3D11_BIND_RENDER_TARGET) &&
        (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) &&
        desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS &&
        ((desc.Width == 960 && desc.Height == 540) ||
         (gameHalfSized && !literalFullSize)) &&
        desc.MipLevels == 1 && desc.ArraySize == 1 &&
        desc.SampleDesc.Count == 1;
      if (fullSizeTarget || halfSizeBlurTarget) {
        role = halfSizeBlurTarget ? RoleHalf : RoleMain;
        desc.Width = halfSizeBlurTarget ? mainWidth / 2 : mainWidth;
        desc.Height = halfSizeBlurTarget ? mainHeight / 2 : mainHeight;
        changed = true;
        if (verboseLogging())
          log("Resizing hard-coded ",
              halfSizeBlurTarget ? "960x540 blur target"
                                 : "1920x1080 target",
              " to ", std::dec, desc.Width, "x", desc.Height,
              " format=", desc.Format);
      }
    }

    // Opt-in shadow-map upscale, twin-allocation flavour: the engine's two
    // 1024x1024 R24G8 shadow maps (caster A and receiver B; a probe
    // established that exactly two such textures exist) are left
    // COMPLETELY untouched so every engine-side size/memory assumption stays
    // valid. Eligible hosts get a separate mod-owned enlarged twin created
    // below (after the host), and the caster DSV / receiver SRV / A->B copy
    // are redirected onto the twins. Anything ambiguous (initial data,
    // staging/CPU-accessible, mips, arrays, multisampled, misc flags)
    // DECLINES the twin and is logged; that host simply keeps the vanilla
    // 1024 path.
    if (shadowMapResolution() > 1024 &&
        originalDesc.Width == 1024 && originalDesc.Height == 1024 &&
        originalDesc.Format == DXGI_FORMAT_R24G8_TYPELESS) {
      createShadowTwin = !pData &&
        originalDesc.Usage == D3D11_USAGE_DEFAULT &&
        originalDesc.CPUAccessFlags == 0 &&
        originalDesc.MiscFlags == 0 &&
        originalDesc.MipLevels == 1 && originalDesc.ArraySize == 1 &&
        originalDesc.SampleDesc.Count == 1;
      if (!createShadowTwin && verboseLogging())
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
  // Written onto the texture itself, so every later question about what this
  // target is gets the answer the resize decision already had.
  if (SUCCEEDED(hr) && role != RoleNone && ppTexture && *ppTexture)
    (*ppTexture)->SetPrivateData(IID_ResolutionRole, sizeof(role), &role);
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
      static std::atomic<bool> reportedShadowResolution{false};
      if (!reportedShadowResolution.exchange(true, std::memory_order_relaxed))
        log("FIXES shadow_resolution=active size=", std::dec,
            shadowRes, "x", shadowRes);
      if (verboseLogging())
        log("SHADOWRES twin created ", std::dec, shadowRes, "x", shadowRes,
            " for host=", *ppTexture, " bind=0x", std::hex,
            originalDesc.BindFlags);
    } else {
      // Fail-safe: no twin means this host silently keeps the vanilla path.
      static std::atomic<uint32_t> reported{0};
      if (logFirstOrVerbose(reported))
        log("SHADOWRES twin creation FAILED hr=0x", std::hex, twinHr,
            " (falling back to 1024 for this map)");
    }
  }
  return hr;
}

// Supersampling: these games bind the swap-chain backbuffer itself as their
// colour render target, so a view the engine asks for over the backbuffer is
// created over the mod's render-resolution target instead. Redirecting at view
// creation rather than at bind time means the binds, the clears and the pre-UI
// SMAA boundary all follow with no further interception, and the real
// backbuffer is touched only by the downscale at Present.
//
// A view desc naming a different format than the backbuffer's is declined
// rather than guessed at: the frame then renders into the backbuffer as it does
// today, which is a lower resolution but never a wrong one.
HRESULT STDMETHODCALLTYPE ID3D11Device_CreateRenderTargetView(
        ID3D11Device*                         pDevice,
        ID3D11Resource*                       pResource,
  const D3D11_RENDER_TARGET_VIEW_DESC*        pDesc,
        ID3D11RenderTargetView**              ppRTView) {
  auto procs = getDeviceProcs(pDevice);
  ID3D11Texture2D* substitute = ssaaRedirectRenderTargetView(pResource, pDesc);
  const HRESULT hr = procs->CreateRenderTargetView(
    pDevice, substitute ? substitute : pResource, pDesc, ppRTView);
  if (substitute) substitute->Release();
  return hr;
}

// Wireframe debug view.
//
// Applied per draw rather than per state-set, because it must not touch the UI
// or movie playback -- both draw with depth testing off, as flat quads over the
// finished scene, and outlining those just fills the screen with rectangles.
// Depth testing being ON is what distinguishes 3D geometry from either.
//
// The game's own rasterizer states are never modified: each gets a wireframe
// twin cached on it as private data, so repeated binds are a lookup rather than
// a state creation, and the twin dies with the state it belongs to.
struct WireframeTracking {
  std::atomic<ID3D11RasterizerState*> requested{nullptr};  // not owned
  std::atomic<bool> active{false};                         // twin bound now
};

WireframeTracking g_wireframeImm;
WireframeTracking g_wireframeDef;

WireframeTracking* wireframeTracking(ID3D11DeviceContext* context) {
  return context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE
    ? &g_wireframeImm : &g_wireframeDef;
}

// The wireframe counterpart of `requested`, or null if one cannot be made.
// Non-owning: the twin is held either by the state it mirrors or by the static
// stand-in for D3D11's solid default, so it outlives this call either way.
ID3D11RasterizerState* wireframeTwinFor(ID3D11DeviceContext* context,
                                        ID3D11RasterizerState* requested) {
  if (requested) {
    ID3D11RasterizerState* twin = nullptr;
    UINT size = sizeof(twin);
    if (SUCCEEDED(requested->GetPrivateData(IID_WireframeState, &size, &twin)) &&
        twin) {
      twin->Release();   // the state we mirror holds the reference
      return twin;
    }
    D3D11_RASTERIZER_DESC desc = {};
    requested->GetDesc(&desc);
    desc.FillMode = D3D11_FILL_WIREFRAME;
    desc.CullMode = D3D11_CULL_NONE;   // see both sides of the shell
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    ID3D11RasterizerState* created = nullptr;
    if (device) {
      if (SUCCEEDED(device->CreateRasterizerState(&desc, &created)) && created) {
        requested->SetPrivateDataInterface(IID_WireframeState, created);
        created->Release();   // now owned by `requested`
      }
      device->Release();
    }
    return created;
  }

  // Null = D3D11's solid default. One shared twin stands in for it.
  static std::atomic<ID3D11RasterizerState*> defaultTwin{nullptr};
  if (ID3D11RasterizerState* existing =
        defaultTwin.load(std::memory_order_acquire))
    return existing;
  D3D11_RASTERIZER_DESC desc = {};
  desc.FillMode = D3D11_FILL_WIREFRAME;
  desc.CullMode = D3D11_CULL_NONE;
  desc.DepthClipEnable = TRUE;
  ID3D11Device* device = nullptr;
  context->GetDevice(&device);
  ID3D11RasterizerState* created = nullptr;
  if (device) {
    if (SUCCEEDED(device->CreateRasterizerState(&desc, &created)) && created) {
      ID3D11RasterizerState* expected = nullptr;
      if (!defaultTwin.compare_exchange_strong(expected, created,
            std::memory_order_acq_rel)) {
        created->Release();
        created = expected;
      }
    }
    device->Release();
  }
  return created;
}

// Called before every draw. Switches the bound rasterizer state only when the
// depth-testing verdict changes, so a frame costs a handful of state sets
// rather than two per draw.
void applyWireframeForDraw(ID3D11DeviceContext* context) {
  if (!debugWireframe())
    return;
  WireframeTracking* wf = wireframeTracking(context);
  const bool want =
    !smaaTrackedState(context)->depthDisabled.load(std::memory_order_relaxed);
  if (want == wf->active.load(std::memory_order_relaxed))
    return;
  wf->active.store(want, std::memory_order_relaxed);
  ID3D11RasterizerState* requested =
    wf->requested.load(std::memory_order_relaxed);
  ID3D11RasterizerState* wanted = want
    ? wireframeTwinFor(context, requested) : requested;
  getContextProcs(context)->RSSetState(context, wanted);
  getRasterState(context)->dirty.store(true, std::memory_order_release);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_RSSetState(
        ID3D11DeviceContext*   pContext,
        ID3D11RasterizerState* pRasterizerState) {
  auto procs = getContextProcs(pContext);
  WireframeTracking* wf = wireframeTracking(pContext);
  wf->requested.store(pRasterizerState, std::memory_order_relaxed);
  // Mid-3D-pass state changes keep the outline; anything else passes through.
  if (debugWireframe() && wf->active.load(std::memory_order_relaxed)) {
    procs->RSSetState(pContext, wireframeTwinFor(pContext, pRasterizerState));
    return;
  }
  procs->RSSetState(pContext, pRasterizerState);
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
    // Deliberately NOT recorded for the supersampling report here: this is the
    // size the game asked for, and the resolution override rewrites it at draw
    // time (see the resizeViewport path), so it is the pre-override value.
    // The applied size is recorded where that substitution happens.
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
  // Alongside the hard-coded 1080p viewport the renderers submit, the game's
  // own requested size counts as full-screen: under supersampling the engine
  // still sizes its main pass from its own settings while the target it draws
  // into is larger. Recorded only when a render resolution is configured, so
  // without one this is the 1080p-only rule it has always been.
  const UINT gameWidth = g_originalSwapWidth.load(std::memory_order_relaxed);
  const UINT gameHeight = g_originalSwapHeight.load(std::memory_order_relaxed);
  const bool splitRender = gameWidth && gameHeight &&
    (gameWidth != g_mainRtWidth.load(std::memory_order_relaxed) ||
     gameHeight != g_mainRtHeight.load(std::memory_order_relaxed));
  const bool fullSizeViewport = viewportCount == 1 &&
    viewport.TopLeftX == 0.0f && viewport.TopLeftY == 0.0f &&
    ((viewport.Width == 1920.0f && viewport.Height == 1080.0f) ||
     (splitRender && viewport.Width == float(gameWidth) &&
      viewport.Height == float(gameHeight)));
  const bool halfSizeViewport = viewportCount == 1 &&
    viewport.TopLeftX == 0.0f && viewport.TopLeftY == 0.0f &&
    ((viewport.Width == 960.0f && viewport.Height == 540.0f) ||
     (splitRender && viewport.Width == float(gameWidth / 2) &&
      viewport.Height == float(gameHeight / 2)));
  const bool fullSizeScissor = scissorCount == 1 &&
    scissor.left == 0 && scissor.top == 0 &&
    ((scissor.right == 1920 && scissor.bottom == 1080) ||
     (splitRender && scissor.right == LONG(gameWidth) &&
      scissor.bottom == LONG(gameHeight)));
  const bool halfSizeScissor = scissorCount == 1 &&
    scissor.left == 0 && scissor.top == 0 &&
    ((scissor.right == 960 && scissor.bottom == 540) ||
     (splitRender && scissor.right == LONG(gameWidth / 2) &&
      scissor.bottom == LONG(gameHeight / 2)));
  // The engine sizes the shadow caster pass viewport from its own texture
  // metadata (still 1024 when the map is enlarged D3D-side). MinDepth/MaxDepth
  // (the caster's 0.5..1.0 depth remap) are preserved by only rewriting
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
      const UINT role = resolutionRole(texture);
      texture->Release();
      const UINT mainWidth = g_mainRtWidth.load(std::memory_order_relaxed);
      const UINT mainHeight = g_mainRtHeight.load(std::memory_order_relaxed);
      // The tag when the mod resized this target, its size otherwise. The
      // fallback keeps every target the mod never touched behaving exactly as
      // before; the tag is what makes the 2x case unambiguous, where
      // main/2 and the display size are the same number.
      const bool fullSizeTarget = role != RoleNone
        ? role == RoleMain
        : (desc.Width == mainWidth && desc.Height == mainHeight);
      const bool halfSizeTarget = role != RoleNone
        ? role == RoleHalf
        : (desc.Width == mainWidth / 2 && desc.Height == mainHeight / 2);
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
        if (verboseLogging() && (n < 16 || n % 1024 == 0))
          log("SHADOWRES caster viewport/scissor 1024 -> ", std::dec,
              desc.Width, " (vp=", resizeViewport,
              " sc=", resizeScissor, ")");
      }
    }
  }

  auto procs = getContextProcs(pContext);
  if (resizeViewport) {
    // Recorded HERE, not in the RSSetViewports hook. The hook sees what the
    // game asked for, which is the pre-override size; this is the size the
    // draw actually happens at, which is the only one that answers whether
    // supersampling is doing anything.
    noteViewportExtent(static_cast<UINT>(viewport.Width),
                       static_cast<UINT>(viewport.Height));
    procs->RSSetViewports(pContext, 1, &viewport);
  }
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
  procs->ClearRenderTargetView(pContext, pRTV, pColor);

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
  procs->ClearDepthStencilView(pContext, pDSV, ClearFlags, Depth, Stencil);
  // Shadow-res twin: keep the enlarged caster map in lockstep with the
  // engine's own (untouched) map -- the engine clears its map at the start of
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
  if (!getResourceInfo(pDstResource, &dstInfo))
    return E_INVALIDARG;

  if (!isCpuWritableResource(&dstInfo))
    return E_INVALIDARG;

  /* Compute source region for the given copy */
  ATFIX_RESOURCE_INFO srcInfo = { };
  if (!getResourceInfo(pSrcResource, &srcInfo))
    return E_INVALIDARG;

  /* A format the size table cannot answer for (block-compressed, or a gap in
   * the table) would run the texel loop below at the wrong row size and the
   * wrong row count. Fail before either resource is mapped and before a shadow
   * is created: both callers fall back to the original GPU copy, which stalls
   * but is correct for every format. Buffers report DXGI_FORMAT_UNKNOWN and
   * measure their box in bytes, so they pass. */
  if (!getFormatPixelSize(dstInfo.Format) || !getFormatPixelSize(srcInfo.Format))
    return E_NOTIMPL;

  const D3D11_BOX srcExtent = getResourceBox(&srcInfo, SrcSubresource);
  D3D11_BOX srcBox = srcExtent;
  D3D11_BOX dstBox = getResourceBox(&dstInfo, DstSubresource);

  if (pSrcBox)
    srcBox = *pSrcBox;

  // D3D11 drops a copy whose destination origin lies outside the destination,
  // one whose source box reaches outside the source, and one with an inverted
  // source box. The CPU path has to reject the same ones: the clamps below are
  // unsigned, so an out-of-range origin wraps instead of clamping and the copy
  // runs at full source width past the end of the mapping. The source side
  // needs the same treatment as the destination side, because only the extent
  // is clamped below and the offset the loop reads from comes straight out of
  // the caller's box. Failing here falls back to the real GPU copy. DstX ==
  // dstBox.right is left to the zero-extent check below, which treats it as a
  // no-op.
  if (srcBox.right < srcBox.left || srcBox.bottom < srcBox.top ||
      srcBox.back < srcBox.front ||
      srcBox.right > srcExtent.right || srcBox.bottom > srcExtent.bottom ||
      srcBox.back > srcExtent.back ||
      DstX > dstBox.right || DstY > dstBox.bottom || DstZ > dstBox.back)
    return E_INVALIDARG;

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
      static std::atomic<uint32_t> reported{0};
      if (logFirstOrVerbose(reported))
        log("Failed to map destination resource, hr=0x", std::hex, hr,
            std::dec, " dim=", dstInfo.Dim, " size=", dstInfo.Width, "x",
            dstInfo.Height, " usage=", dstInfo.Usage);
    }
    return hr;
  }

  ID3D11Resource* shadowResource = nullptr;

  if (!isCpuReadableResource(&srcInfo)) {
    shadowResource = getOrCreateShadowResource(pContext, pSrcResource);

    /* Null means creation failed (out of memory, device removed, or a
     * dimension we do not handle) and has already been logged. Failing here
     * is the graceful path: the caller falls back to a real GPU copy, which
     * costs the stall this function exists to avoid but is still correct. */
    if (!shadowResource) {
      procs->Unmap(pContext, pDstResource, DstSubresource);
      return E_FAIL;
    }

    hr = procs->Map(pContext, shadowResource, SrcSubresource, D3D11_MAP_READ, 0, &srcSr);

    if (FAILED(hr)) {
      shadowResource->Release();

      static std::atomic<uint32_t> reported{0};
      if (logFirstOrVerbose(reported))
        log("Failed to map shadow resource, hr 0x", std::hex, hr);
      procs->Unmap(pContext, pDstResource, DstSubresource);
      return hr;
    }
  } else {
    hr = procs->Map(pContext, pSrcResource, SrcSubresource, D3D11_MAP_READ, 0, &srcSr);

    if (FAILED(hr)) {
      static std::atomic<uint32_t> reported{0};
      if (logFirstOrVerbose(reported))
        log("Failed to map source resource, hr=0x", std::hex, hr,
            std::dec, " dim=", srcInfo.Dim, " size=", srcInfo.Width, "x",
            srcInfo.Height, " usage=", srcInfo.Usage);
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

  if (isTraceBackbuffer(pDstResource)) {
    static std::atomic<uint32_t> n{0};
    if (n.fetch_add(1, std::memory_order_relaxed) < 4) {
      D3D11_TEXTURE2D_DESC s = { };
      const bool haveSrc = texture2DDesc(pSrcResource, &s);
      log("PRESENTTRACE copy-into-backbuffer op=resource src=", pSrcResource,
          " src_size=", std::dec, haveSrc ? s.Width : 0, "x",
          haveSrc ? s.Height : 0, " (scenario B transfer)");
    }
  }


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
        if (verboseLogging() && (n < 16 || n % 4096 == 0))
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

  if (isTraceBackbuffer(pDstResource)) {
    static std::atomic<uint32_t> n{0};
    if (n.fetch_add(1, std::memory_order_relaxed) < 4) {
      D3D11_TEXTURE2D_DESC s = { };
      const bool haveSrc = texture2DDesc(pSrcResource, &s);
      log("PRESENTTRACE copy-into-backbuffer op=subresource src=", pSrcResource,
          " src_size=", std::dec, haveSrc ? s.Width : 0, "x",
          haveSrc ? s.Height : 0, " dstXY=", DstX, ",", DstY,
          " (scenario B transfer)");
    }
  }

  D3D11_BOX scaledBox = { };
  // Held until the copy has been issued: the substituted source is AddRef'd by
  // ssaaAcquireColor and must outlive the call that consumes it.
  ID3D11Texture2D* substitutedSource = nullptr;
  const UINT mainWidth = g_mainRtWidth.load(std::memory_order_relaxed);
  const UINT mainHeight = g_mainRtHeight.load(std::memory_order_relaxed);
  // The source box is hard-coded 1080p; under a render/display split the game
  // may instead ask for its own configured size. Either way it is expanded to
  // the main target both ends actually live in.
  const UINT gameWidth = g_originalSwapWidth.load(std::memory_order_relaxed);
  const UINT gameHeight = g_originalSwapHeight.load(std::memory_order_relaxed);
  const bool gameSizedBox = gameWidth && gameHeight &&
    (gameWidth != mainWidth || gameHeight != mainHeight) && pSrcBox &&
    pSrcBox->right == gameWidth && pSrcBox->bottom == gameHeight;
  if (mainWidth > 1920 && mainHeight > 1080 && pSrcBox &&
      DstSubresource == 0 && SrcSubresource == 0 &&
      DstX == 0 && DstY == 0 && DstZ == 0 &&
      pSrcBox->left == 0 && pSrcBox->top == 0 && pSrcBox->front == 0 &&
      ((pSrcBox->right == 1920 && pSrcBox->bottom == 1080) || gameSizedBox) &&
      pSrcBox->back == 1) {
    D3D11_TEXTURE2D_DESC dstDesc = { };
    D3D11_TEXTURE2D_DESC srcDesc = { };
    // The format is matched by family, not exactly. The engine's own textures
    // are TYPELESS, but under supersampling the source can be the mod's own
    // render-resolution colour target, which is created with the backbuffer's
    // format (UNORM) instead. Demanding TYPELESS therefore rejected exactly the
    // case that needs expanding most, and the copy stayed at the display size:
    // a dialogue whose scene fills the top-left quarter of the frame and is
    // black everywhere else.
    const auto isBgra8 = [](DXGI_FORMAT format) {
      return format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
             format == DXGI_FORMAT_B8G8R8A8_UNORM ||
             format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    };
    if (texture2DDesc(pDstResource, &dstDesc) &&
        texture2DDesc(pSrcResource, &srcDesc) &&
        dstDesc.Width == mainWidth && dstDesc.Height == mainHeight &&
        srcDesc.Width == mainWidth && srcDesc.Height == mainHeight &&
        isBgra8(dstDesc.Format) && isBgra8(srcDesc.Format)) {
      const UINT fromWidth = pSrcBox->right;
      const UINT fromHeight = pSrcBox->bottom;
      scaledBox = *pSrcBox;
      scaledBox.right = mainWidth;
      scaledBox.bottom = mainHeight;
      pSrcBox = &scaledBox;
      const UINT marker = 1;
      pDstResource->SetPrivateData(
        IID_DialogSnapshotResource, sizeof(marker), &marker);
      if (verboseLogging())
        log("Expanded dialogue snapshot copy from ", std::dec,
            fromWidth, "x", fromHeight, " to ",
            mainWidth, "x", mainHeight);
    } else if (texture2DDesc(pDstResource, &dstDesc) &&
               texture2DDesc(pSrcResource, &srcDesc) &&
               dstDesc.Width == mainWidth && dstDesc.Height == mainHeight &&
               isBgra8(dstDesc.Format) && isBgra8(srcDesc.Format) &&
               ssaaIsBackbuffer(pSrcResource)) {
      // The engine takes its screen snapshots -- the frozen scene behind a shop
      // or conversation menu -- by copying FROM the swap chain's backbuffer,
      // which it fetches with GetBuffer. That is why the render-target redirect
      // never catches this: the redirect replaces VIEWS the engine creates over
      // the backbuffer, and a copy needs no view.
      //
      // Under supersampling the backbuffer holds the finished frame already
      // DOWNSCALED to the display size, while the snapshot texture has been
      // resized to the render size. Copying one into the other lands a
      // 1920x1080 image in the top-left corner of a 3840x2160 texture, and the
      // menu then draws that texture full-screen: the scene appears in a
      // quarter of the frame with black around it.
      //
      // So the source is substituted for the mod's own render-resolution
      // colour target, which holds the same frame before it was downscaled --
      // the copy then fills the snapshot, at full resolution rather than the
      // display's. Nothing changes when supersampling is off: ssaaAcquireColor
      // returns null and this falls through to the vanilla copy.
      //
      // Not the 2x size ambiguity that caused the neighbouring bugs: this one
      // fails at every factor, because the backbuffer is always the display
      // size and the snapshot is always the render size.
      if (ID3D11Texture2D* fullResolution = ssaaAcquireColor()) {
        substitutedSource = fullResolution;
        pSrcResource = fullResolution;
        scaledBox.left = 0;
        scaledBox.top = 0;
        scaledBox.front = 0;
        scaledBox.right = mainWidth;
        scaledBox.bottom = mainHeight;
        scaledBox.back = 1;
        pSrcBox = &scaledBox;
        const UINT marker = 1;
        pDstResource->SetPrivateData(
          IID_DialogSnapshotResource, sizeof(marker), &marker);
        static std::atomic<bool> saidSo { false };
        if (verboseLogging() && !saidSo.exchange(true))
          log("Snapshot copy taken from the render-resolution frame instead of"
              " the downscaled backbuffer (", std::dec, mainWidth, "x",
              mainHeight, ")");
      }
    } else if (texture2DDesc(pDstResource, &dstDesc) &&
               texture2DDesc(pSrcResource, &srcDesc)) {
      // A copy that looked like the dialogue snapshot but failed one of the
      // checks above. Logged once, with the values, because the symptom of a
      // missed expansion is a mostly black screen and nothing else says why.
      static std::atomic<bool> reported { false };
      if (!reported.exchange(true))
        log("Dialogue snapshot copy NOT expanded: src ", std::dec,
            srcDesc.Width, "x", srcDesc.Height, " fmt ", srcDesc.Format,
            ", dst ", dstDesc.Width, "x", dstDesc.Height, " fmt ",
            dstDesc.Format, ", main ", mainWidth, "x", mainHeight);
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
        if (verboseLogging() && (n < 16 || n % 4096 == 0))
          log("SHADOWRES mirrored A->B copy on twins");
        srcTwin->Release();
      }
      dstTwin->Release();
    }
  }


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
      procs->CopySubresourceRegion(pContext,
        dstShadow,    DstSubresource, DstX, DstY, DstZ,
        pSrcResource, SrcSubresource, pSrcBox);
    }

    dstShadow->Release();
  }

  // Released only here: every copy above may have consumed it.
  if (substitutedSource)
    substitutedSource->Release();
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
    const HRESULT hr = shadowResource->QueryInterface(IID_PPV_ARGS(&shadowBuffer));
    shadowResource->Release();

    // The shadow of a buffer is a buffer, so the query answers; a failure would
    // otherwise reach Release through a null pointer.
    if (SUCCEEDED(hr) && shadowBuffer) {
      procs->CopyStructureCount(pContext, shadowBuffer, DstOffset, pSrcUav);
      shadowBuffer->Release();
    }
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
  smaaFireOnSceneRtRelease(pContext, ppRTVs, RTVCount);
  smaaSceneBoundary(pContext, RTVCount, ppRTVs, pDSV);
  trackSmaaRenderTargets(pContext, RTVCount, ppRTVs, pDSV);
  presentTraceRenderTargets(RTVCount, ppRTVs, pDSV);
  getRasterState(pContext)->dirty.store(true, std::memory_order_release);

  // Shadow-res twin: redirect the depth-only caster pass onto the enlarged
  // twin DSV so casters render at high resolution. Only depth-only binds are
  // redirected -- if a color RTV is bound alongside (never observed) the
  // sizes could not match, so we fail safe to the engine's own
  // 1024 pass and log it.
  ID3D11DepthStencilView* shadowTwinDsv = nullptr;
  if (shadowMapResolution() > 1024 && pDSV) {
    shadowTwinDsv = getShadowResTwinDsv(pDSV);
    if (shadowTwinDsv) {
      static std::atomic<uint32_t> redirectLogs{0};
      const uint32_t n = redirectLogs.fetch_add(1, std::memory_order_relaxed);
      if (RTVCount == 0 || !ppRTVs || !ppRTVs[0]) {
        pDSV = shadowTwinDsv;
        if (verboseLogging() && (n < 16 || n % 4096 == 0))
          log("SHADOWRES caster DSV redirected to twin");
      } else {
        if (verboseLogging() && (n < 16 || n % 256 == 0))
          log("SHADOWRES DSV redirect SKIP: color RTV bound with shadow map");
        shadowTwinDsv->Release();
        shadowTwinDsv = nullptr;
      }
    }
  }

  procs->OMSetRenderTargets(pContext, RTVCount, ppRTVs, pDSV);
  if (shadowTwinDsv) shadowTwinDsv->Release();
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
  trackSmaaRenderTargets(pContext, RTVCount, ppRTVs, pDSV);
  getRasterState(pContext)->dirty.store(true, std::memory_order_release);

  procs->OMSetRenderTargetsAndUnorderedAccessViews(pContext,
    RTVCount, ppRTVs, pDSV, UAVIndex, UAVCount, ppUAVs, pUAVClearValues);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_OMSetDepthStencilState(
        ID3D11DeviceContext*      pContext,
        ID3D11DepthStencilState*  pDepthStencilState,
        UINT                      StencilRef) {
  auto procs = getContextProcs(pContext);
  trackSmaaDepthState(pContext, pDepthStencilState);
  procs->OMSetDepthStencilState(
    pContext, pDepthStencilState, StencilRef);
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

  const void* effectiveData = pData;
  uint8_t gateHoldCopy[880];
  if (cbCaptureEnabled() && pData && !pBox && Subresource == 0) {
    D3D11_BUFFER_DESC desc = {};
    if (isConstantBuffer(pResource, &desc)) {
      if (arlandInCinematicBattle()) {
        // Dim-hold: keep the faded light $Params at 1.0 during the cut-in.
        // pData is const (DEFAULT buffer), so patch a copy and pass that on.
        if (cutinDimHoldEnabled() && dimHoldEligibleSize(desc.ByteWidth)) {
          // Totori's largest dim-carrying layout is 13024 bytes (skinned toon
          // VS $Params) -- too big for the stack; a thread-local scratch keeps
          // the substitution allocation-free per call.
          static thread_local std::vector<uint8_t> dimHoldScratch;
          dimHoldScratch.assign(static_cast<const uint8_t*>(pData),
            static_cast<const uint8_t*>(pData) + desc.ByteWidth);
          if (dimHoldPatch(dimHoldScratch.data(), desc.ByteWidth))
            effectiveData = dimHoldScratch.data();
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

  updateMirroredSubresource<ID3D11Resource>(
    pResource, effectiveData,
    [&](ID3D11Resource* resource, const void* data) {
      procs->UpdateSubresource(pContext, resource,
        Subresource, pBox, data, RowPitch, SlicePitch);
    },
    [](ID3D11Resource* resource) { return getShadowResource(resource); },
    [](ID3D11Resource* resource) { resource->Release(); });
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

struct DirtyShadowUpload {
  const ContextProcs* procs;
  ID3D11DeviceContext* context;
  ID3D11Resource* resource;
  ID3D11Resource* shadow;
  UINT subresource;
  D3D11_MAP destinationMapType;
  const ATFIX_RESOURCE_INFO* info;
  D3D11_MAPPED_SUBRESOURCE destination = {};
  D3D11_MAPPED_SUBRESOURCE source = {};
  HRESULT hr = S_OK;

  bool mapDestination() {
    hr = procs->Map(context, resource, subresource,
      destinationMapType, 0, &destination);
    return SUCCEEDED(hr);
  }

  bool mapSource() {
    hr = procs->Map(context, shadow, subresource,
      D3D11_MAP_READ, 0, &source);
    return SUCCEEDED(hr);
  }

  void copy() {
    copyMappedSubresource(info, subresource, &destination, &source);
  }

  void unmapSource() {
    procs->Unmap(context, shadow, subresource);
  }

  void unmapDestination() {
    procs->Unmap(context, resource, subresource);
  }

  void requeue() {
    markShadowDirty(resource, subresource);
  }
};

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
    /* tryCpuCopy rejects an unsized format before a shadow can be created, so
     * the format test here cannot fire. It stays because copyMappedSubresource
     * below has no safe answer for one: by the time it runs both subresources
     * are mapped, the destination map has discarded its contents, and a GPU
     * copy is illegal for a DYNAMIC destination. */
    if (!shadow || !getResourceInfo(resource, &info) ||
        (info.Dim != D3D11_RESOURCE_DIMENSION_BUFFER &&
         !getFormatPixelSize(info.Format))) {
      if (shadow)
        shadow->Release();
      resource->Release();
      continue;
    }

    const D3D11_MAP mapType = info.Usage == D3D11_USAGE_DYNAMIC
      ? D3D11_MAP_WRITE_DISCARD
      : D3D11_MAP_WRITE;
    DirtyShadowUpload upload = {
      procs, pContext, resource, shadow, subresource, mapType, &info };
    const ShadowUploadResult result = uploadDirtyShadow(upload);
    if (result == ShadowUploadResult::Copied) {
      if (transitionTraceEnabled()) {
        const uint32_t mip = info.Mips ? subresource % info.Mips : 0;
        const uint64_t width = std::max(info.Width >> mip, 1u);
        const uint64_t height = std::max(info.Height >> mip, 1u);
        const uint64_t depth = std::max(info.Depth >> mip, 1u);
        g_transitionShadowFlushBytes.fetch_add(
          width * height * depth * getFormatPixelSize(info.Format),
          std::memory_order_relaxed);
      }
    } else if (result == ShadowUploadResult::SourceMapFailed) {
      static std::atomic<uint32_t> reportedShadowUpload{0};
      if (logFirstOrVerbose(reportedShadowUpload))
        log("Failed to map a shadow resource for upload, hr 0x",
            std::hex, upload.hr);
    } else {
      static std::atomic<uint32_t> reportedDestinationUpload{0};
      if (logFirstOrVerbose(reportedDestinationUpload))
        log("Failed to map a destination resource for upload, hr 0x",
            std::hex, upload.hr);
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
    ? reinterpret_cast<uintptr_t>(arlandReturnAddress()) : 0;
  if (atlasReconcileEnabled() &&
      (MapType == D3D11_MAP_WRITE_DISCARD ||
       MapType == D3D11_MAP_WRITE_NO_OVERWRITE ||
       MapType == D3D11_MAP_WRITE) &&
      isMutableFontAtlas(pResource))
    g_atlasWriteMaps.fetch_add(1, std::memory_order_relaxed);
  auto procs = getContextProcs(pContext);
  if (!pResource || !isImmediatecontext(pContext)) {
    mapKindTimer.setBranch(0);
    const HRESULT hr = procs->Map(
      pContext, pResource, Subresource, MapType, MapFlags, pMappedResource);
    if (SUCCEEDED(hr)) {
      captureCbMap(pContext, pResource, Subresource, pMappedResource);
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
      captureCbMap(pContext, pResource, Subresource, pMappedResource);
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
    // The Map above already filled the caller's struct, and the pointer in it is
    // dead once the shadow is unmapped. Clear it so a caller that ignores the
    // HRESULT faults here rather than writing through it into whatever now owns
    // that memory.
    if (pMappedResource)
      *pMappedResource = D3D11_MAPPED_SUBRESOURCE { };
    return E_FAIL;
  }
      captureCbMap(pContext, pResource, Subresource, pMappedResource);
  return hr;
}

void STDMETHODCALLTYPE ID3D11DeviceContext_Unmap(
        ID3D11DeviceContext*       pContext,
        ID3D11Resource*            pResource,
        UINT                       Subresource) {
  auto procs = getContextProcs(pContext);
  captureCbUnmap(pContext, pResource, Subresource);
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

void STDMETHODCALLTYPE ID3D11DeviceContext_Draw(
        ID3D11DeviceContext* pContext, UINT VertexCount,
        UINT StartVertexLocation) {
  auto procs = getContextProcs(pContext);
  flushDirtyShadows(pContext);
  updateViewportScissor(pContext);
  gateHoldAtDraw(pContext);
  smaaDrawBoundary(pContext);
  applyWireframeForDraw(pContext);

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

  procs->Draw(pContext, VertexCount, StartVertexLocation);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_DrawIndexed(
        ID3D11DeviceContext* pContext, UINT IndexCount,
        UINT StartIndexLocation, INT BaseVertexLocation) {
  auto procs = getContextProcs(pContext);
  flushDirtyShadows(pContext);
  updateViewportScissor(pContext);
  gateHoldAtDraw(pContext);
  smaaDrawBoundary(pContext);
  applyWireframeForDraw(pContext);

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
  procs->DrawIndexed(pContext, IndexCount, StartIndexLocation,
    BaseVertexLocation);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_DrawInstanced(
        ID3D11DeviceContext* pContext, UINT VertexCountPerInstance,
        UINT InstanceCount, UINT StartVertexLocation,
        UINT StartInstanceLocation) {
  auto procs = getContextProcs(pContext);
  flushDirtyShadows(pContext);
  updateViewportScissor(pContext);
  gateHoldAtDraw(pContext);
  smaaDrawBoundary(pContext);
  applyWireframeForDraw(pContext);
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
  smaaDrawBoundary(pContext);
  applyWireframeForDraw(pContext);
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

void STDMETHODCALLTYPE ID3D11DeviceContext_PSSetShaderResources(
        ID3D11DeviceContext* pContext, UINT StartSlot, UINT NumViews,
        ID3D11ShaderResourceView* const* ppShaderResourceViews) {
  auto procs = getContextProcs(pContext);
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
      if (verboseLogging() && (n < 16 || n % 4096 == 0))
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
  allInstalled = hookProc(object, #iface "::" #proc, &table->proc, \
    &iface ## _ ## proc, index) && allInstalled

template<typename T>
bool hookProc(void* pObject, const char* pName, T** ppOrig, T* pHook,
              uint32_t index) {
  void** vtbl = *reinterpret_cast<void***>(pObject);

  // A vtable entry normally points into the module the vtable itself lives in
  // (the D3D11 runtime, or DXVK). When it does not, another injector has
  // already swapped this slot, and the detour below patches that injector's
  // function rather than the runtime's. Calls still flow (the foreign hook
  // forwards), so proceed -- but say so: without this line the install logs a
  // routine success in exactly the stacked-mod configuration most likely to
  // behave strangely. Special K makes the same module comparison before its
  // vtable-resolved detours (SK_ValidateVFTableAddress). First mismatch always
  // logs; the rest of a stacked install repeats only under verbose logging.
  HMODULE vtblModule = nullptr;
  HMODULE entryModule = nullptr;
  const DWORD moduleFlags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
  if (GetModuleHandleExW(moduleFlags,
        reinterpret_cast<LPCWSTR>(vtbl), &vtblModule) &&
      GetModuleHandleExW(moduleFlags,
        reinterpret_cast<LPCWSTR>(vtbl[index]), &entryModule) &&
      vtblModule != entryModule) {
    static std::atomic<uint32_t> foreignEntries{0};
    if (logFirstOrVerbose(foreignEntries))
      log("Vtable entry for ", pName, " @ ", vtbl[index],
          " lives outside its vtable's module; detouring another injector's"
          " hook");
  }

  MH_STATUS mh = MH_CreateHook(vtbl[index],
    reinterpret_cast<void*>(pHook),
    reinterpret_cast<void**>(ppOrig));

  if (mh) {
    if (mh != MH_ERROR_ALREADY_CREATED) {
      log("Failed to create hook for ", pName, ": ", MH_StatusToString(mh));
      // Target is left unpatched on this failure, so the vtable entry is still
      // the real function: point the table at it rather than leaving a null.
      // Hooks call through the table for other methods (Draw ->
      // flushDirtyShadows -> procs->Map), so one failed slot would crash a hook
      // that installed fine. Excludes ALREADY_CREATED, where a detour of ours
      // is on the target and this entry would recurse into our own hook.
      *ppOrig = reinterpret_cast<T*>(vtbl[index]);
    }
    return mh == MH_ERROR_ALREADY_CREATED;
  }

  mh = MH_EnableHook(vtbl[index]);

  if (mh) {
    log("Failed to enable hook for ", pName, ": ", MH_StatusToString(mh));
    return false;
  }

  if (verboseLogging())
    log("Created hook for ", pName, " @ ", reinterpret_cast<void*>(pHook));
  return true;
}

void hookDevice(ID3D11Device* pDevice) {
  std::lock_guard lock(g_hookMutex);

  if (g_installedHooks & HOOK_DEVICE)
    return;

  if (verboseLogging())
    log("Hooking device ", pDevice);

  DeviceProcs* procs = &g_deviceProcs;
  bool allInstalled = true;
  HOOK_PROC(ID3D11Device, pDevice, procs, 3,  CreateBuffer);
  HOOK_PROC(ID3D11Device, pDevice, procs, 27, CreateDeferredContext);
  HOOK_PROC(ID3D11Device, pDevice, procs, 4,  CreateTexture1D);
  HOOK_PROC(ID3D11Device, pDevice, procs, 5,  CreateTexture2D);
  HOOK_PROC(ID3D11Device, pDevice, procs, 6,  CreateTexture3D);
  HOOK_PROC(ID3D11Device, pDevice, procs, 12, CreateVertexShader);
  HOOK_PROC(ID3D11Device, pDevice, procs, 15, CreatePixelShader);
  if (ssaaRequested())
    HOOK_PROC(ID3D11Device, pDevice, procs, 9, CreateRenderTargetView);

  g_installedHooks |= HOOK_DEVICE;
  log("FIXES d3d11_device_hooks=",
      allInstalled ? "active" : "partial_failure");
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

  if (verboseLogging())
    log("Hooking context ", pContext);

  bool allInstalled = true;
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
  if (debugWireframe())
    HOOK_PROC(ID3D11DeviceContext, pContext, procs, 43, RSSetState);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 44, RSSetViewports);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 45, RSSetScissorRects);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 33, OMSetRenderTargets);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 34, OMSetRenderTargetsAndUnorderedAccessViews);
  // Feeds the wireframe view's UI/movie exclusion and BOTH pre-UI boundaries,
  // so any of the three needs it. Not just the depth-state boundary: the
  // scene-target injector's first-UI-draw trigger reads the same depthDisabled
  // flag, and this hook is the only thing that ever writes it. Gate this on one
  // pass while the boundary is open for another and the flag stays false
  // forever, which kills the depth-state boundary outright and leaves the
  // scene-target one firing on its bind-away backstop alone, possibly after the
  // UI has drawn onto the scene target.
  if (preUiBoundaryNeeded() || debugWireframe())
    HOOK_PROC(ID3D11DeviceContext, pContext, procs, 36, OMSetDepthStencilState);
  HOOK_PROC(ID3D11DeviceContext, pContext, procs, 48, UpdateSubresource);

  g_installedHooks |= flag;
  log("FIXES d3d11_",
      flag == HOOK_IMM_CTX ? "immediate_context_hooks="
                           : "deferred_context_hooks=",
      allInstalled ? "active" : "partial_failure");

  /* Immediate and deferred contexts share one vtable, so the second context to
     reach hookProc gets MH_ERROR_ALREADY_CREATED and its originals are NOT
     repopulated. The immediate context is created at device creation and hooked
     first, so we copy its captured originals to the deferred table here. Guard on
     the immediate table actually being populated: if a deferred context were ever
     hooked first, this pass's hooks would all no-op (already created) and copying
     an empty g_immContextProcs would clobber the good deferred originals with
     nulls. In the normal (immediate-first) order the guard is always true. */
  if ((flag & HOOK_IMM_CTX) && g_immContextProcs.Draw)
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
