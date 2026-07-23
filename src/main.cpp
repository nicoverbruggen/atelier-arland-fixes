// Derived from Philip Rebohle's atelier-sync-fix; see LICENSE (zlib).
#include <iostream>

#include "crash_log.h"
#include "menu_fix.h"
#include "smaa.h"
#include "sync_fix.h"
#include "util.h"

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

  installCrashLogger();

  HMODULE libD3D11 = LoadLibraryExA("d3d11_proxy.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);

  if (libD3D11) {
    log("Using d3d11_proxy.dll");
  } else {
    std::array<char, MAX_PATH + 1> path = { };

    if (!GetSystemDirectoryA(path.data(), MAX_PATH))
      return D3D11Proc();

    std::strncat(path.data(), "\\d3d11.dll", MAX_PATH);
    log("Using ", path.data());
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

  log("D3D11CreateDevice             @ ", reinterpret_cast<void*>(d3d11Proc.D3D11CreateDevice));
  log("D3D11CreateDeviceAndSwapChain @ ", reinterpret_cast<void*>(d3d11Proc.D3D11CreateDeviceAndSwapChain));
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
  const char* blob = std::getenv("ARLAND_CUTIN_BLOB");
  return (trace && trace[0] != '0') ||
    (blob && blob[0] != '0') ||
    arland::frameAtlasCacheEnabled() ||
    arland::battleShadowRestoreActive();
}

// Present must be hooked whenever the transition trace OR SMAA needs it.
bool presentHookNeeded() {
  return menuTransitionTraceEnabled() || atfix::smaaEnabled();
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
  // catch. Negligible cost; the trend before a hang is the diagnostic value.
  {
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
  atfix::cutinDrawContactBlobs(swapChain);
  atfix::smaaApply(swapChain);        // Present-time path (only if pre-UI off)
  const HRESULT result = originalPresent(swapChain, syncInterval, flags);
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
    log("Failed to create transition Present hook: ",
      MH_StatusToString(status));
    return;
  }
  status = MH_EnableHook(vtable[8]);
  if (status) {
    log("Failed to enable transition Present hook: ",
      MH_StatusToString(status));
    return;
  }
  log("Created transition Present hook @ ", vtable[8]);
}

HRESULT STDMETHODCALLTYPE tracedCreateSwapChain(
    IDXGIFactory* factory, IUnknown* device,
    DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain) {
  const HRESULT result = originalCreateSwapChain(
    factory, device, desc, swapChain);
  if (SUCCEEDED(result) && swapChain && *swapChain)
    hookSwapChain(*swapChain);
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
      if (status)
        log("Failed to hook IDXGIFactory::CreateSwapChain: ",
          MH_StatusToString(status));
      else
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
    if (atfix::applyResolutionOverride(&swapChainDesc))
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
    if (ppSwapChain && *ppSwapChain)
      atfix::hookSwapChain(*ppSwapChain);
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
      break;

    case DLL_PROCESS_DETACH:
      MH_Uninitialize();
      break;
  }

  return TRUE;
}

}
