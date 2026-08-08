// Derived from Philip Rebohle's atelier-sync-fix; see LICENSE (zlib).
#include <iostream>

#include "config.h"
#include "crash_log.h"
#include "menu_fix.h"
#include "pad_notify_trace.h"
#include "path_util.h"
#include "sharpen.h"
#include "smaa.h"
#include "supersample.h"
#include "sync_fix.h"
#include "util.h"
#include "version.h"
#include "version_git.h"
#include "window_background.h"
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

// Beside the game executable, the same anchor the ini uses, rather than the
// working directory a launcher happens to leave us in. Built here rather than
// inside Log because this global is constructed before anything else in the
// DLL and the path has to exist by then; GetModuleFileNameA and string work are
// both safe that early. Falls back to the working directory if the module path
// cannot be read, which is the behaviour this replaces.
const char* logPath() {
  static char path[MAX_PATH] = { };
  const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (!n || n >= MAX_PATH ||
      !replaceFileName(path, sizeof(path), "arland-fix.log"))
    return "arland-fix.log";
  return path;
}

Log log(logPath());

/** Load system D3D11 DLL and return entry points */
using PFN_D3D11CreateDevice = HRESULT (__stdcall *) (
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
  UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

// Forwarded untouched. The mod has nothing to do with D3D11-on-12, but a tool
// injected alongside it can import the name statically, and a missing static
// import stops the process before any code runs: no window, no log, nothing to
// report. 3Dmigoto records exactly that with OpenXR Toolkit against a proxy
// that exported a subset. The signature is left opaque because nothing here
// inspects the arguments.
using PFN_D3D11On12CreateDevice = HRESULT (__stdcall *) (
        IUnknown*, UINT, const D3D_FEATURE_LEVEL*, UINT, IUnknown**, UINT,
        UINT, ID3D11Device**, ID3D11DeviceContext**, D3D_FEATURE_LEVEL*);

using PFN_D3D11CreateDeviceAndSwapChain = HRESULT (__stdcall *) (
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
  UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
  D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

struct D3D11Proc {
  PFN_D3D11CreateDevice             D3D11CreateDevice             = nullptr;
  PFN_D3D11CreateDeviceAndSwapChain D3D11CreateDeviceAndSwapChain = nullptr;
  PFN_D3D11On12CreateDevice         D3D11On12CreateDevice         = nullptr;
};

D3D11Proc loadSystemD3D11() {
  static mutex initMutex;
  static D3D11Proc d3d11Proc;
  // Both entry points are published together, and by a flag that is not one of
  // them: the two members are assigned on separate lines, so a reader testing
  // the first one can take the fast path while the second is still null and
  // hand E_FAIL back to the game. The games create their device on one thread,
  // but nothing guarantees these exports only one caller: ReShade guards its
  // own D3D11 entry points against concurrent re-entry from DXGI's internal
  // device creation (its thread_local g_in_dxgi_runtime), and the cost of
  // assuming a single thread here is a silent E_FAIL at device creation.
  static std::atomic<bool> ready = { false };

  if (ready.load(std::memory_order_acquire))
    return d3d11Proc;

  std::lock_guard lock(initMutex);

  if (ready.load(std::memory_order_relaxed))
    return d3d11Proc;

  log("Atelier Arland Fix version ", ARLAND_FIX_VERSION,
      " build=", ARLAND_FIX_GIT,
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

    // strncat's third argument is how much it may append, not how much room is
    // left, so it cannot express this bound. Measure instead: the returned
    // length is what GetSystemDirectoryA wrote, and the append needs room for
    // the separator, the name and the terminator.
    const UINT length = GetSystemDirectoryA(path.data(), MAX_PATH);
    const char suffix[] = "\\d3d11.dll";

    if (!length || length + sizeof(suffix) > path.size())
      return D3D11Proc();

    std::memcpy(path.data() + length, suffix, sizeof(suffix));
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
  // Resolved here rather than through a .def forwarder: this DLL is itself
  // named d3d11.dll, so a forwarder would name itself and loop.
  d3d11Proc.D3D11On12CreateDevice = reinterpret_cast<PFN_D3D11On12CreateDevice>(
    GetProcAddress(libD3D11, "D3D11On12CreateDevice"));

  // The real d3d11.dll always exports both, so a null here means a chain-loaded
  // d3d11_proxy.dll that is not a D3D11 implementation. The exports return
  // E_FAIL on a null member; without this line that failure has no explanation
  // in the log.
  if (!d3d11Proc.D3D11CreateDevice || !d3d11Proc.D3D11CreateDeviceAndSwapChain)
    log("Loaded D3D11 module is missing a device-creation export;"
        " device creation will fail");

  arland::initializeGameHooks();

  if (atfix::verboseLogging()) {
    log("D3D11CreateDevice             @ ",
      reinterpret_cast<void*>(d3d11Proc.D3D11CreateDevice));
    log("D3D11CreateDeviceAndSwapChain @ ",
      reinterpret_cast<void*>(d3d11Proc.D3D11CreateDeviceAndSwapChain));
  }
  // Last, after both members are assigned. The two failure returns above leave
  // the flag clear, so a transient load failure still retries.
  ready.store(true, std::memory_order_release);
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

// Present must be hooked whenever the transition trace, the frame-atlas cache,
// the per-frame battle ticks, SMAA, the supersampling downscale, borderless mode
// or the frame-time log needs it. The first three arrive through
// menuTransitionTraceEnabled above.
//
// Order matters: battleShadowRestoreActive() reads g_battleAddrs, which the
// game-hook fan-out sets. Both device-creation routes run the fan-out before
// this decision. Taking this decision earlier would leave Rorona's battle
// shadows without their Present-driven half while the install still logs active.
bool presentHookNeeded() {
  return menuTransitionTraceEnabled() ||
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
  // see the crash analysis below) leaves a trail in arland-fix.log
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

  atfix::maintainBorderlessWindow();   // re-applies only if the game restyled
  atfix::noteSceneAnchor(swapChain);         // re-anchor: survives ResizeBuffers
  atfix::notePresentBackbuffer(swapChain);   // ARLAND_PRESENT_TRACE diagnostic
  // Loading the shader compiler from a draw or bind detour deadlocks on the
  // loader lock; from the frame tick nothing is held. Idempotent, and a no-op
  // when sharpening is off.
  atfix::sharpenPreload();
  atfix::smaaApply(swapChain);        // Present-time path (only if pre-UI off)
  atfix::ssaaDownscale(swapChain);    // supersampling: render res -> backbuffer
  const HRESULT result = originalPresent(
    swapChain, presentInterval(syncInterval), flags);
  // Record a lost device once — the post-mortem a present-time hang/TDR leaves.
  // Kept as a passive diagnostic: it names the fault when a transition-teardown
  // race removes the device.
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

// The two exports below are deliberately independent: each forwards to its own
// system counterpart and neither is implemented in terms of the other. Special K
// and ReShade both build D3D11CreateDevice on their own
// D3D11CreateDeviceAndSwapChain; 3Dmigoto once did the reverse and records the
// two recursing into each other without bound when such proxies stack. The
// duplication between these functions is what keeps this proxy out of that
// failure, so do not merge them.
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

// Pass-through, and nothing else. It exists so the name resolves: a tool
// injected alongside this one can import it statically, and the loader fails
// the whole process when a static import is missing, before any code runs.
// Nothing in this mod touches D3D11-on-12, so the arguments go straight out
// again untouched.
DLLEXPORT HRESULT __stdcall D3D11On12CreateDevice(
        IUnknown*             pDevice,
        UINT                  Flags,
  const D3D_FEATURE_LEVEL*    pFeatureLevels,
        UINT                  FeatureLevels,
        IUnknown**            ppCommandQueues,
        UINT                  NumQueues,
        UINT                  NodeMask,
        ID3D11Device**        ppDevice,
        ID3D11DeviceContext** ppImmediateContext,
        D3D_FEATURE_LEVEL*    pChosenFeatureLevel) {
  auto proc = atfix::loadSystemD3D11();

  if (!proc.D3D11On12CreateDevice)
    return E_NOTIMPL;

  return proc.D3D11On12CreateDevice(pDevice, Flags, pFeatureLevels,
    FeatureLevels, ppCommandQueues, NumQueues, NodeMask, ppDevice,
    ppImmediateContext, pChosenFeatureLevel);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
      MH_Initialize();
      // Both of these hook a window API and have to be in place before the
      // game registers its class and creates its window, which is why they
      // run here rather than at device creation. Skipped in the documented
      // full pass-through mode.
      if (!atfix::modDisabled()) {
        atfix::installWindowTitleFix();
        atfix::installWindowBackgroundFix();
      }
      break;

    case DLL_PROCESS_DETACH:
      // Only on dynamic unload, where the detours have to be removed before the
      // code they jump into unmaps. On process exit lpvReserved is non-null and
      // the other threads have already been terminated; a thread killed inside
      // any MH_* call still holds MinHook's lock flag, and MH_Uninitialize
      // would then spin forever in EnterSpinLock (vendor/minhook/src/hook.c),
      // which has no timeout, leaving the process unable to finish closing.
      // The pad-notification trace owns a thread and two windows, and the same
      // reasoning applies to it: on process exit its pump has already been
      // terminated, so posting to its windows and waiting for it would wait for
      // a thread that cannot answer.
      if (lpvReserved == nullptr) {
        atfix::stopPadNotifyTrace();
        MH_Uninitialize();
      }
      break;
  }

  return TRUE;
}

}
