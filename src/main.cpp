// Derived from Philip Rebohle's atelier-sync-fix; see LICENSE (zlib).
#include <iostream>

#include "config.h"
#include "crash_log.h"
#include "menu_fix.h"
#include "smaa.h"
#include "supersample.h"
#include "sync_fix.h"
#include "util.h"
#include "version.h"
#include "window_mode.h"
#include "window_title.h"

#include <psapi.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#ifdef _MSC_VER
  #define DLLEXPORT
#else
  #define DLLEXPORT __declspec(dllexport)
#endif

namespace atfix {

Log log("arland-fix.log");

/** Load system D3D11 DLL and return entry points */
using PFN_D3D11CreateDevice = HRESULT (__stdcall *) (
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
  UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

using PFN_D3D11CreateDeviceAndSwapChain = HRESULT (__stdcall *) (
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
  UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
  D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

struct D3D11Proc {
  PFN_D3D11CreateDevice             D3D11CreateDevice             = nullptr;
  PFN_D3D11CreateDeviceAndSwapChain D3D11CreateDeviceAndSwapChain = nullptr;
};

D3D11Proc loadSystemD3D11() {
  static mutex initMutex;
  static D3D11Proc d3d11Proc;

  if (d3d11Proc.D3D11CreateDevice)
    return d3d11Proc;

  std::lock_guard lock(initMutex);

  if (d3d11Proc.D3D11CreateDevice)
    return d3d11Proc;

  log("Atelier Arland Fix version ", ARLAND_FIX_VERSION,
      " verbose=", atfix::verboseLogging() ? 1 : 0);

  // Skipped when the mod is stood down: the point of that switch is a process
  // this DLL has not touched, and an installed exception filter is a thing it
  // has touched.
  if (atfix::modDisabled())
    log("ARLAND_DISABLE is set: forwarding Direct3D only, nothing installed");
  else
    installCrashLogger();
  // Before any hook reports what it did, so the log opens with the settings
  // that produced everything below it.
  atfix::logConfiguration();

  HMODULE libD3D11 = LoadLibraryExA("d3d11_proxy.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);

  if (libD3D11) {
    log("D3D11 forwarding: d3d11_proxy.dll");
  } else {
    std::array<char, MAX_PATH + 1> path = { };

    if (!GetSystemDirectoryA(path.data(), MAX_PATH))
      return D3D11Proc();

    std::strncat(path.data(), "\\d3d11.dll", MAX_PATH);
    log("D3D11 forwarding: system d3d11.dll");
    if (atfix::verboseLogging())
      log("D3D11 system path: ", path.data());
    libD3D11 = LoadLibraryA(path.data());

    if (!libD3D11) {
      log("Failed to load d3d11.dll (", path.data(), ")");
      return D3D11Proc();
    }
  }

  d3d11Proc.D3D11CreateDevice = reinterpret_cast<PFN_D3D11CreateDevice>(
    GetProcAddress(libD3D11, "D3D11CreateDevice"));
  d3d11Proc.D3D11CreateDeviceAndSwapChain = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(
    GetProcAddress(libD3D11, "D3D11CreateDeviceAndSwapChain"));

  arland::initializeGameHooks();

  if (atfix::verboseLogging()) {
    log("D3D11CreateDevice             @ ",
      reinterpret_cast<void*>(d3d11Proc.D3D11CreateDevice));
    log("D3D11CreateDeviceAndSwapChain @ ",
      reinterpret_cast<void*>(d3d11Proc.D3D11CreateDeviceAndSwapChain));
  }
  return d3d11Proc;
}

using PFN_IDXGISwapChain_Present = HRESULT (STDMETHODCALLTYPE *) (
  IDXGISwapChain*, UINT, UINT);

PFN_IDXGISwapChain_Present originalPresent = nullptr;
using PFN_IDXGIFactory_CreateSwapChain = HRESULT (STDMETHODCALLTYPE *) (
  IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
PFN_IDXGIFactory_CreateSwapChain originalCreateSwapChain = nullptr;
mutex presentHookMutex;
std::atomic<int64_t> previousPresentNanos = 0;
// The address our Present detour was placed on: the swap chain's vtable slot as
// it read when we hooked. Compared against the slot later to see whether
// anything installed itself in front of us (reportPresentHookNeighbours).
void* hookedPresentTarget = nullptr;
void reportPresentHookNeighbours(IDXGISwapChain* swapChain);   // below

bool menuTransitionTraceEnabled() {
  const char* trace = std::getenv("ARLAND_MENU_TRANSITION_TRACE");
  return (trace && trace[0] != '0') ||
    arland::frameAtlasCacheEnabled() ||
    arland::battleShadowRestoreActive();
}

// Frame-time logging, off unless asked for. It is a measuring tool rather than
// something a normal session needs, and leaving it on would put a line in every
// user's log every ten seconds for the benefit of nobody reading it.
//
// Present has to be hooked for it, which is why it appears in presentHookNeeded
// below: when it IS on, an A/B usually means turning the other features off, and
// the hook would otherwise go with them and take the measurement with it.
bool perfLogEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_PERF_LOG");
    if (value)
      return value[0] != '0';
    // Otherwise it follows [Diagnostics] VerboseLogging, so the launcher's own
    // checkbox turns it on. That matters more than it sounds: a tester cannot
    // easily set an environment variable, least of all through Steam, and this
    // is exactly the measurement worth asking a tester for.
    return atfix::verboseLogging();
  }();
  return enabled;
}

// Present must be hooked whenever the transition trace, MSAA safety resolve,
// SMAA, supersampling downscale, borderless mode or frame-time log needs it.
bool presentHookNeeded() {
  return menuTransitionTraceEnabled() || atfix::msaaSamples() > 1 ||
    atfix::smaaEnabled() ||
    atfix::presentTraceEnabled() || atfix::ssaaRequested() ||
    atfix::borderlessWindow() || perfLogEnabled();
}

// Replace the game's present interval for a session: 0 turns vsync off so an
// external limiter (MangoHud's fps_limit) can pace frames itself, 2/3 present
// every Nth refresh. Diagnostic only — needed because the games expose no vsync
// setting, so measuring behaviour at a frame rate that is not a divisor of the
// display refresh is otherwise impossible. Unset leaves the game's own value,
// which is what every normal run uses: presenting unsynchronized is what tears,
// so nothing but an explicit opt-in may change it.
UINT presentInterval(UINT gameInterval) {
  static const int override = [] {
    const char* value = std::getenv("ARLAND_PRESENT_INTERVAL");
    return value ? std::atoi(value) : -1;
  }();
  return override >= 0 ? UINT(override) : gameInterval;
}

HRESULT STDMETHODCALLTYPE tracedPresent(
    IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
  const auto started = std::chrono::steady_clock::now();
  const int64_t startedNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
    started.time_since_epoch()).count();
  const int64_t previous = previousPresentNanos.exchange(
    startedNanos, std::memory_order_relaxed);
  // Passive crash probe: record process memory every ~10s so the runaway-
  // allocation hang in dense battles (the KTGL sound reclaim-starvation leak —
  // see the crash analysis in TECHNICAL.md) leaves a trail in arland-fix.log
  // even though it hangs rather than throwing an exception the post-mortem could
  // catch. Opt-in via [Diagnostics] VerboseLogging so the default log stays quiet.
  if (atfix::verboseLogging()) {
    static std::atomic<int64_t> lastMemNanos{0};
    int64_t prevMem = lastMemNanos.load(std::memory_order_relaxed);
    if (startedNanos - prevMem >= 10'000'000'000LL &&
        lastMemNanos.compare_exchange_strong(prevMem, startedNanos,
          std::memory_order_relaxed)) {
      PROCESS_MEMORY_COUNTERS pmc = {};
      if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        log("MEM working-set=", std::dec,
          static_cast<unsigned>(pmc.WorkingSetSize >> 20u),
          "MB peak=", static_cast<unsigned>(pmc.PeakWorkingSetSize >> 20u),
          "MB commit=", static_cast<unsigned>(pmc.PagefileUsage >> 20u), "MB");
    }
  }
  // Frame-time heartbeat. Verbose/opt-in: the questions this project keeps
  // having to answer are "what does this setting cost?" and "is it slower than
  // it was?", and both are unanswerable from a report that says "it felt fine".
  // A line every ten seconds makes any two diagnostic runs comparable without
  // the tester installing an overlay or knowing what one is.
  //
  // The average alone hides the thing people actually notice, so the worst
  // frame in the window is reported alongside it: a steady 60 with a 90 ms
  // hitch reads very differently from a steady 60.
  //
  // Costs one timestamp subtraction per frame, which is already being taken
  // above for other reasons.
  if (previous && perfLogEnabled()) {
    static std::atomic<int64_t> windowStartNanos{0};
    static std::atomic<int64_t> windowFrames{0};
    static std::atomic<int64_t> windowWorstNanos{0};

    const int64_t frameNanos = startedNanos - previous;
    windowFrames.fetch_add(1, std::memory_order_relaxed);
    int64_t worst = windowWorstNanos.load(std::memory_order_relaxed);
    while (frameNanos > worst && !windowWorstNanos.compare_exchange_weak(
        worst, frameNanos, std::memory_order_relaxed)) {}

    int64_t windowStart = windowStartNanos.load(std::memory_order_relaxed);
    if (!windowStart) {
      windowStartNanos.compare_exchange_strong(windowStart, startedNanos,
        std::memory_order_relaxed);
    } else if (startedNanos - windowStart >= 10'000'000'000LL &&
               windowStartNanos.compare_exchange_strong(windowStart,
                 startedNanos, std::memory_order_relaxed)) {
      const int64_t frames = windowFrames.exchange(0, std::memory_order_relaxed);
      const int64_t worstFrame =
        windowWorstNanos.exchange(0, std::memory_order_relaxed);
      const int64_t elapsed = startedNanos - windowStart;
      if (frames > 0 && elapsed > 0) {
        // Integer arithmetic with one decimal place: the log has no float
        // formatting and a rounded whole number hides a 59-vs-60 difference.
        const int64_t fpsTenths = frames * 10'000'000'000LL / elapsed;
        const int64_t averageMicros = elapsed / frames / 1000;
        log("PERF fps=", std::dec, fpsTenths / 10, ".", fpsTenths % 10,
            " avg=", averageMicros / 1000, ".",
            (averageMicros % 1000) / 100, "ms",
            " worst=", worstFrame / 1'000'000, "ms",
            " frames=", frames);
      }
    }
  }

  // Once, a few hundred frames in: an overlay that hooks Present lazily has
  // installed itself by then, and this is the line that says whether it sits
  // in front of us. Verbose only.
  {
    static std::atomic<uint32_t> presents{0};
    if (presents.fetch_add(1, std::memory_order_relaxed) == 300)
      reportPresentHookNeighbours(swapChain);
  }

  atfix::maintainBorderlessWindow();   // re-applies only if the game restyled
  atfix::noteSceneAnchor(swapChain);         // re-anchor: survives ResizeBuffers
  atfix::notePresentBackbuffer(swapChain);   // ARLAND_PRESENT_TRACE diagnostic
  atfix::resolveMsaaBeforePresent(swapChain);  // twin -> host, before any read
  atfix::smaaApply(swapChain);        // Present-time path (only if pre-UI off)
  atfix::ssaaDownscale(swapChain);    // supersampling: render res -> backbuffer
  const HRESULT result = originalPresent(
    swapChain, presentInterval(syncInterval), flags);
  // Put the frame back in the buffer the present rotated in, so the backbuffer
  // holds the current picture for the rest of the frame instead of the last one
  // presented. Only matters to whoever reads the backbuffer outside this hook —
  // Steam's screenshot capture among them. See ssaaRefreshBackbuffer.
  if (SUCCEEDED(result))
    atfix::ssaaRefreshBackbuffer();
  // Record a lost device once — the post-mortem a present-time hang/TDR leaves.
  // Kept as a passive diagnostic: it names the fault when a transition-teardown
  // race removes the device (see the crash analysis in TECHNICAL.md).
  if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
    static std::atomic<bool> loggedLoss{false};
    if (!loggedLoss.exchange(true, std::memory_order_relaxed)) {
      HRESULT reason = result;
      ID3D11Device* device = nullptr;
      if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device))) && device) {
        reason = device->GetDeviceRemovedReason();
        device->Release();
      }
      log("Present device lost: result=0x", std::hex,
        static_cast<uint32_t>(result), " reason=0x",
        static_cast<uint32_t>(reason), std::dec);
    }
  }
  atfix::smaaResetFrame();            // arm the pre-UI SMAA latch for next frame
  const auto finished = std::chrono::steady_clock::now();
  const uint64_t durationMicros = uint64_t(
    std::chrono::duration_cast<std::chrono::microseconds>(
      finished - started).count());
  const uint64_t intervalMicros = previous > 0 && startedNanos >= previous
    ? uint64_t(startedNanos - previous) / 1000 : 0;
  atfix::traceTransitionD3DFrame(intervalMicros);
  arland::traceMenuPresent(durationMicros, intervalMicros);
  return result;
}

// Which module owns a code address, by base name ("gameoverlayrenderer.dll",
// "dxgi.dll", ...), or "?" when it cannot be resolved. Writes into the caller's
// buffer rather than a static one: both addresses are named in a single log
// statement, and a shared buffer would print the same name twice.
const char* moduleOwning(void* address, char (&name)[MAX_PATH]) {
  name[0] = '\0';
  HMODULE module = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCSTR>(address), &module) || !module)
    return "?";
  char path[MAX_PATH] = { };
  if (!GetModuleFileNameA(module, path, sizeof(path)))
    return "?";
  const char* base = path;
  for (const char* p = path; *p; ++p)
    if (*p == '\\' || *p == '/') base = p + 1;
  lstrcpynA(name, base, MAX_PATH);
  return name;
}

// The module a code address belongs to, for identity rather than for printing.
// nullptr when the address is in memory no module owns, which is what a hooking
// library's own allocated relays and trampolines look like.
HMODULE moduleHandleOf(void* address) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCSTR>(address), &module))
    return nullptr;
  return module;
}

// Whether a range can be read without faulting. The addresses below are decoded
// out of whatever bytes another process-wide hook happened to leave behind, so
// nothing here may assume they point anywhere in particular.
bool readable(const void* address, size_t bytes) {
  MEMORY_BASIC_INFORMATION info = { };
  if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT)
    return false;
  if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))
    return false;
  const auto start = static_cast<const uint8_t*>(address);
  const auto regionEnd = static_cast<const uint8_t*>(info.BaseAddress) +
    info.RegionSize;
  return start + bytes <= regionEnd;
}

// Follow a chain of unconditional jumps to the code that actually runs.
//
// Both hooks in play here work by overwriting the first bytes of the target
// function with a jump, so "who runs first" is not a pointer comparison but a
// question of where that jump lands. Several hops are followed because the
// first one rarely lands on the detour itself: MinHook places a relay next to
// the target when the detour is further than a rel32 can reach, and other
// hooking libraries do the same, so a single hop lands in an anonymous
// allocation that says nothing about who owns the hook.
//
// Only the forms these patches actually emit are decoded. Anything else ends
// the walk and the address so far is returned -- this names a module in a log
// line, so stopping early is a worse answer, never a wrong action.
void* followJumps(void* address, int maxHops = 4) {
  for (int hop = 0; hop < maxHops && address; ++hop) {
    const auto code = static_cast<const uint8_t*>(address);
    if (!readable(code, 16))
      return address;
    if (code[0] == 0xE9) {           // jmp rel32
      int32_t displacement = 0;
      std::memcpy(&displacement, code + 1, sizeof(displacement));
      address = const_cast<uint8_t*>(code) + 5 + displacement;
    } else if (code[0] == 0xEB) {    // jmp rel8
      address = const_cast<uint8_t*>(code) + 2 + int8_t(code[1]);
    } else if ((code[0] == 0xFF && code[1] == 0x25) ||
               (code[0] == 0x48 && code[1] == 0xFF && code[2] == 0x25)) {
      // jmp qword [rip+disp32], with or without the redundant REX.W. The slot
      // it reads through is data, so it is bounds-checked separately.
      const int prefix = code[0] == 0x48 ? 1 : 0;
      int32_t displacement = 0;
      std::memcpy(&displacement, code + prefix + 2, sizeof(displacement));
      const void** slot = reinterpret_cast<const void**>(
        const_cast<uint8_t*>(code) + prefix + 6 + displacement);
      if (!readable(slot, sizeof(*slot)))
        return address;
      address = const_cast<void*>(*slot);
    } else if (code[0] == 0x48 && code[1] == 0xB8 &&
               code[10] == 0xFF && code[11] == 0xE0) {
      // mov rax, imm64 ; jmp rax -- the absolute form, used where a relay
      // cannot be placed within rel32 range of the target.
      void* destination = nullptr;
      std::memcpy(&destination, code + 2, sizeof(destination));
      address = destination;
    } else {
      return address;              // not a jump: this is the code that runs
    }
  }
  return address;
}

// Who else is on this swap chain's Present, and are they inside or outside us.
//
// The Steam overlay hooks Present too, and the ordering decides both what a
// Steam screenshot contains and whether its overlay is visible at all: our
// composite clears the backbuffer and paints the frame over it, so an overlay
// drawn before we run is erased, while one drawn after survives.
//
// Neither hook is visible in the vtable. MinHook and the overlay both patch the
// first bytes of the function the slot points at, and whoever patches LAST runs
// first -- the overlay installs itself lazily, on an early Present, which is
// after we hook the swap chain the game just created. So the question is
// answered by decoding the prologue and seeing whose code that jump reaches:
// ours means nothing has been installed over us, a foreign module means that
// module's Present runs before ours and its drawing is what we erase. The slot
// is still compared as well, for a hook that does replace it.
//
// Verbose only; called once, a couple of seconds in, so a lazily-installed
// overlay hook has had time to appear.
void reportPresentHookNeighbours(IDXGISwapChain* swapChain) {
  if (!atfix::verboseLogging() || !swapChain || !hookedPresentTarget)
    return;
  void** vtable = *reinterpret_cast<void***>(swapChain);
  void* slot = vtable[8];
  void* entered = followJumps(slot);
  const HMODULE ours = moduleHandleOf(reinterpret_cast<void*>(&tracedPresent));
  const HMODULE entryOwner = moduleHandleOf(entered);
  // Landing anywhere but our own module means somebody is in front of us: an
  // unowned allocation is a hooking library's relay, and it is not ours or the
  // walk would have reached this DLL.
  const bool first = entryOwner && entryOwner == ours;
  char slotName[MAX_PATH];
  char enteredName[MAX_PATH];
  log("Present chain: slot ", slot, " in ",
      moduleOwning(slot, slotName),
      slot == hookedPresentTarget ? "" : " (REPLACED since we hooked)",
      ", entered code at ", entered, " in ",
      moduleOwning(entered, enteredName), first
        ? " -- ours runs first, so an overlay drawing after us survives"
        : " -- that code runs before ours, so our composite erases whatever"
          " it drew into the backbuffer");
}

void hookSwapChain(IDXGISwapChain* swapChain) {
  if (!swapChain || !presentHookNeeded())
    return;
  std::lock_guard lock(presentHookMutex);
  if (originalPresent)
    return;
  void** vtable = *reinterpret_cast<void***>(swapChain);
  MH_STATUS status = MH_CreateHook(vtable[8],
    reinterpret_cast<void*>(&tracedPresent),
    reinterpret_cast<void**>(&originalPresent));
  if (status && status != MH_ERROR_ALREADY_CREATED) {
    static std::atomic<bool> reported{false};
    if (atfix::verboseLogging() ||
        !reported.exchange(true, std::memory_order_relaxed))
      log("Failed to create transition Present hook: ",
        MH_StatusToString(status));
    return;
  }
  status = MH_EnableHook(vtable[8]);
  if (status) {
    static std::atomic<bool> reported{false};
    if (atfix::verboseLogging() ||
        !reported.exchange(true, std::memory_order_relaxed))
      log("Failed to enable transition Present hook: ",
        MH_StatusToString(status));
    return;
  }
  hookedPresentTarget = vtable[8];
  if (atfix::verboseLogging())
    log("Created transition Present hook @ ", vtable[8]);
}

HRESULT STDMETHODCALLTYPE tracedCreateSwapChain(
    IDXGIFactory* factory, IUnknown* device,
    DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain) {
  // The trilogy reaches the swap chain both ways: D3D11CreateDeviceAndSwapChain
  // in some configurations, and D3D11CreateDevice followed by this factory call
  // in others. The resolution override has to apply on either route, or the
  // internal targets get resized while the backbuffer keeps the size the game
  // asked for.
  if (desc) {
    atfix::applyResolutionOverride(desc);
    atfix::prepareBorderlessSwapChain(desc);
  }
  const HRESULT result = originalCreateSwapChain(
    factory, device, desc, swapChain);
  if (SUCCEEDED(result) && swapChain && *swapChain) {
    atfix::ssaaNoteSwapChain(*swapChain);
    atfix::noteSceneAnchor(*swapChain);
    atfix::applyBorderlessWindow(*swapChain);
    hookSwapChain(*swapChain);
  }
  return result;
}

void hookFactoryForSwapChain(ID3D11Device* device) {
  if (!device || !presentHookNeeded())
    return;
  IDXGIDevice* dxgiDevice = nullptr;
  IDXGIAdapter* adapter = nullptr;
  IDXGIFactory* factory = nullptr;
  HRESULT result = device->QueryInterface(
    IID_IDXGIDevice, reinterpret_cast<void**>(&dxgiDevice));
  if (SUCCEEDED(result))
    result = dxgiDevice->GetAdapter(&adapter);
  if (SUCCEEDED(result))
    result = adapter->GetParent(
      IID_IDXGIFactory, reinterpret_cast<void**>(&factory));
  if (FAILED(result) || !factory) {
    static std::atomic<bool> reported{false};
    if (atfix::verboseLogging() ||
        !reported.exchange(true, std::memory_order_relaxed))
      log("Failed to obtain DXGI factory for transition trace: ",
        std::hex, result, std::dec);
  } else {
    std::lock_guard lock(presentHookMutex);
    if (!originalCreateSwapChain) {
      void** vtable = *reinterpret_cast<void***>(factory);
      MH_STATUS status = MH_CreateHook(vtable[10],
        reinterpret_cast<void*>(&tracedCreateSwapChain),
        reinterpret_cast<void**>(&originalCreateSwapChain));
      if (!status || status == MH_ERROR_ALREADY_CREATED)
        status = MH_EnableHook(vtable[10]);
      if (status) {
        static std::atomic<bool> reported{false};
        if (atfix::verboseLogging() ||
            !reported.exchange(true, std::memory_order_relaxed))
          log("Failed to hook IDXGIFactory::CreateSwapChain: ",
            MH_StatusToString(status));
      }
      else if (atfix::verboseLogging())
        log("Created transition CreateSwapChain hook @ ", vtable[10]);
    }
  }
  if (factory)
    factory->Release();
  if (adapter)
    adapter->Release();
  if (dxgiDevice)
    dxgiDevice->Release();
}

}

extern "C" {

DLLEXPORT HRESULT __stdcall D3D11CreateDevice(
        IDXGIAdapter*         pAdapter,
        D3D_DRIVER_TYPE       DriverType,
        HMODULE               Software,
        UINT                  Flags,
  const D3D_FEATURE_LEVEL*    pFeatureLevels,
        UINT                  FeatureLevels,
        UINT                  SDKVersion,
        ID3D11Device**        ppDevice,
        D3D_FEATURE_LEVEL*    pFeatureLevel,
        ID3D11DeviceContext** ppImmediateContext) {
  if (ppDevice)
    *ppDevice = nullptr;

  if (ppImmediateContext)
    *ppImmediateContext = nullptr;

  auto proc = atfix::loadSystemD3D11();

  if (!proc.D3D11CreateDevice)
    return E_FAIL;

  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;

  HRESULT hr = (*proc.D3D11CreateDevice)(pAdapter, DriverType, Software,
    Flags, pFeatureLevels, FeatureLevels, SDKVersion, &device, pFeatureLevel,
    &context);

  if (FAILED(hr))
    return hr;

  if (arland::initializeGameHooks()) {
    atfix::hookDevice(device);
    atfix::hookContext(context);
    atfix::hookFactoryForSwapChain(device);
  }

  if (ppDevice) {
    device->AddRef();
    *ppDevice = device;
  }

  if (ppImmediateContext) {
    context->AddRef();
    *ppImmediateContext = context;
  }

  device->Release();
  context->Release();
  return hr;
}

DLLEXPORT HRESULT __stdcall D3D11CreateDeviceAndSwapChain(
        IDXGIAdapter*         pAdapter,
        D3D_DRIVER_TYPE       DriverType,
        HMODULE               Software,
        UINT                  Flags,
  const D3D_FEATURE_LEVEL*    pFeatureLevels,
        UINT                  FeatureLevels,
        UINT                  SDKVersion,
  const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
        IDXGISwapChain**      ppSwapChain,
        ID3D11Device**        ppDevice,
        D3D_FEATURE_LEVEL*    pFeatureLevel,
        ID3D11DeviceContext** ppImmediateContext) {
  if (ppDevice)
    *ppDevice = nullptr;

  if (ppImmediateContext)
    *ppImmediateContext = nullptr;

  if (ppSwapChain)
    *ppSwapChain = nullptr;

  auto proc = atfix::loadSystemD3D11();

  if (!proc.D3D11CreateDeviceAndSwapChain)
    return E_FAIL;

  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  DXGI_SWAP_CHAIN_DESC swapChainDesc = { };
  if (pSwapChainDesc && arland::initializeGameHooks()) {
    swapChainDesc = *pSwapChainDesc;
    const bool resized = atfix::applyResolutionOverride(&swapChainDesc);
    atfix::prepareBorderlessSwapChain(&swapChainDesc);
    if (resized || atfix::borderlessWindow())
      pSwapChainDesc = &swapChainDesc;
  }

  HRESULT hr = (*proc.D3D11CreateDeviceAndSwapChain)(pAdapter, DriverType, Software,
    Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain,
    &device, pFeatureLevel, &context);

  if (FAILED(hr))
    return hr;

  if (arland::initializeGameHooks()) {
    atfix::hookDevice(device);
    atfix::hookContext(context);
    if (ppSwapChain && *ppSwapChain) {
      // Before the game can create a view over the backbuffer, so supersampling
      // owns every one of them but the downscale's own.
      atfix::ssaaNoteSwapChain(*ppSwapChain);
      atfix::noteSceneAnchor(*ppSwapChain);
      atfix::applyBorderlessWindow(*ppSwapChain);
      atfix::hookSwapChain(*ppSwapChain);
    }
  }

  if (ppDevice) {
    device->AddRef();
    *ppDevice = device;
  }

  if (ppImmediateContext) {
    context->AddRef();
    *ppImmediateContext = context;
  }

  device->Release();
  context->Release();
  return hr;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
      MH_Initialize();
      // Hook ANSI title APIs before the game's window is created, except in
      // the documented full pass-through mode.
      if (!atfix::modDisabled())
        atfix::installWindowTitleFix();
      break;

    case DLL_PROCESS_DETACH:
      MH_Uninitialize();
      break;
  }

  return TRUE;
}

}
