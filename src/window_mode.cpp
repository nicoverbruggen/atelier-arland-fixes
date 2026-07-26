// SPDX-License-Identifier: MIT
//
// Borderless windowed mode. See window_mode.h for what it is and why.
//
// Two halves. Before the swap chain is created the description is forced to
// windowed, because a swap chain created for exclusive fullscreen owns the
// display mode and restyling its window afterwards would fight it. After the
// swap chain exists the window's decorations are removed and it is sized to
// cover the monitor it sits on.
//
// The style is re-checked each frame rather than set once: the engine restyles
// its own window in places (and so does Wine's window manager integration), and
// a single application at startup does not always survive.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>

#include "config.h"
#include "hook_util.h"   // installMinHookDetour
#include "log.h"
#include "window_mode.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

std::atomic<HWND> g_window { nullptr };
std::atomic<bool> g_applied { false };

// The styles a borderless window keeps. WS_POPUP with no border or caption;
// WS_VISIBLE because the window is already shown by the time we get here.
constexpr LONG_PTR kBorderlessStyle = WS_POPUP | WS_VISIBLE;
constexpr LONG_PTR kBorderlessExStyle = WS_EX_APPWINDOW;

// The monitor the window is currently on, so a second display works and the
// window does not jump to the primary one.
bool monitorBounds(HWND window, RECT* bounds) {
  HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  if (!monitor)
    return false;
  MONITORINFO info = { };
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoA(monitor, &info))
    return false;
  *bounds = info.rcMonitor;   // full monitor, not rcWork: the taskbar is covered
  return true;
}

bool alreadyBorderless(HWND window, const RECT& bounds) {
  const LONG_PTR style = GetWindowLongPtrA(window, GWL_STYLE);
  if (style & (WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME))
    return false;
  RECT current = { };
  if (!GetWindowRect(window, &current))
    return false;
  return current.left == bounds.left && current.top == bounds.top &&
         current.right == bounds.right && current.bottom == bounds.bottom;
}

// Cross-thread SetWindowPos sends messages synchronously to the owning thread,
// so if a restyle never sticks, re-applying it every frame turns into hammering
// the game's message loop from the render thread. Give up after a few attempts
// and say so rather than risk wedging the game.
constexpr uint32_t kMaxRestyleAttempts = 8;
std::atomic<uint32_t> g_attempts { 0 };

void restyle(HWND window, bool firstTime) {
  RECT bounds = { };
  if (!monitorBounds(window, &bounds)) {
    if (firstTime)
      log("Borderless: could not determine the window's monitor; not applied");
    return;
  }
  if (alreadyBorderless(window, bounds)) {
    if (firstTime)
      log("Borderless: window is already borderless and monitor-sized"
        " (", std::dec, bounds.right - bounds.left, "x",
        bounds.bottom - bounds.top, "); nothing to do");
    return;
  }
  const uint32_t attempt = g_attempts.fetch_add(1, std::memory_order_relaxed);
  if (attempt >= kMaxRestyleAttempts) {
    if (attempt == kMaxRestyleAttempts)
      log("Borderless: the window keeps reverting after ", std::dec, attempt,
        " attempts; leaving it alone from here");
    return;
  }
  if (firstTime && verboseLogging())
    log("Borderless: applying to window ", window, ", current style 0x",
      std::hex, GetWindowLongPtrA(window, GWL_STYLE), std::dec,
      ", owned by ",
      GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId()
        ? "this thread" : "another thread");

  SetWindowLongPtrA(window, GWL_STYLE, kBorderlessStyle);
  SetWindowLongPtrA(window, GWL_EXSTYLE, kBorderlessExStyle);
  // SWP_FRAMECHANGED makes the style change take effect; no Z-order change, so
  // the window does not force itself above others.
  //
  // SWP_ASYNCWINDOWPOS when the window belongs to another thread, which it
  // normally does: this is called from the render thread, while the game pumps
  // messages elsewhere. Without it SetWindowPos sends WM_NCCALCSIZE and friends
  // synchronously to that thread and blocks until it answers -- which wedges
  // both threads if it is itself waiting on the renderer.
  UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED;
  if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId())
    flags |= SWP_ASYNCWINDOWPOS;
  SetWindowPos(window, nullptr, bounds.left, bounds.top,
    bounds.right - bounds.left, bounds.bottom - bounds.top, flags);

  if (!firstTime)
    return;
  const LONG width = bounds.right - bounds.left;
  const LONG height = bounds.bottom - bounds.top;
  log("FIXES borderless=active size=", std::dec, width, "x", height,
    " position=", bounds.left, ",", bounds.top);
  // The window always fills the monitor, so a display resolution smaller than
  // it is scaled up to fit. That is legitimate (it is how you downsample from a
  // higher render resolution to a cheaper present one) but it looks like a
  // blurry bug if it was not intended, and the key name gives no hint of it.
  UINT displayWidth = 0;
  UINT displayHeight = 0;
  if (displayResolution(&displayWidth, &displayHeight) &&
      (LONG(displayWidth) != width || LONG(displayHeight) != height))
    log("Borderless: DisplayWidth/DisplayHeight is ", std::dec, displayWidth,
      "x", displayHeight, " but the monitor is ", width, "x", height,
      " -- the image is scaled to fit. Match them to avoid that.");
}

// The game's own FullScreen setting is neutralised at the swap chain (forced
// windowed) and at the window (restyled every frame). The one route left is the
// display mode itself: anything that switches the desktop resolution would
// change the screen out from under a window we have sized to it. Refuse those
// while borderless is on, and report success so a caller that checks the result
// carries on as though the mode had changed.
using PFN_ChangeDisplaySettingsExA = LONG (WINAPI*)(LPCSTR, DEVMODEA*, HWND,
  DWORD, LPVOID);
using PFN_ChangeDisplaySettingsExW = LONG (WINAPI*)(LPCWSTR, DEVMODEW*, HWND,
  DWORD, LPVOID);
PFN_ChangeDisplaySettingsExA originalChangeDisplaySettingsExA = nullptr;
PFN_ChangeDisplaySettingsExW originalChangeDisplaySettingsExW = nullptr;

void noteRefusedModeChange() {
  static std::atomic<uint32_t> refused { 0 };
  if (refused.fetch_add(1, std::memory_order_relaxed) == 0)
    log("Borderless: refused a display-mode change"
        " (the game's own fullscreen setting is ignored in this mode)");
}

LONG WINAPI tracedChangeDisplaySettingsExA(LPCSTR device, DEVMODEA* mode,
    HWND window, DWORD flags, LPVOID param) {
  if (borderlessWindow() && mode) {
    noteRefusedModeChange();
    return DISP_CHANGE_SUCCESSFUL;
  }
  return originalChangeDisplaySettingsExA(device, mode, window, flags, param);
}

LONG WINAPI tracedChangeDisplaySettingsExW(LPCWSTR device, DEVMODEW* mode,
    HWND window, DWORD flags, LPVOID param) {
  if (borderlessWindow() && mode) {
    noteRefusedModeChange();
    return DISP_CHANGE_SUCCESSFUL;
  }
  return originalChangeDisplaySettingsExW(device, mode, window, flags, param);
}

// A null DEVMODE means "return to the registry-stored mode", which is what a
// game does on exit; that is allowed through, so nothing is left switched.
void installModeChangeGuard() {
  static bool installed = false;
  if (installed || !borderlessWindow())
    return;
  installed = true;
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (!user32)
    return;
  auto* changeA = reinterpret_cast<void*>(
    GetProcAddress(user32, "ChangeDisplaySettingsExA"));
  auto* changeW = reinterpret_cast<void*>(
    GetProcAddress(user32, "ChangeDisplaySettingsExW"));
  if (changeA)
    installMinHookDetour(static_cast<BYTE*>(changeA),
      reinterpret_cast<void*>(&tracedChangeDisplaySettingsExA),
      reinterpret_cast<void**>(&originalChangeDisplaySettingsExA));
  if (changeW)
    installMinHookDetour(static_cast<BYTE*>(changeW),
      reinterpret_cast<void*>(&tracedChangeDisplaySettingsExW),
      reinterpret_cast<void**>(&originalChangeDisplaySettingsExW));
}

}  // namespace

void prepareBorderlessSwapChain(DXGI_SWAP_CHAIN_DESC* desc) {
  if (!borderlessWindow() || !desc)
    return;
  g_window.store(desc->OutputWindow, std::memory_order_relaxed);
  installModeChangeGuard();
  if (!desc->Windowed) {
    desc->Windowed = TRUE;
    static std::atomic<bool> reported{false};
    if (verboseLogging() ||
        !reported.exchange(true, std::memory_order_relaxed))
      log("Borderless: swap chain forced to windowed"
          " (the game asked for exclusive fullscreen)");
  }
}

void applyBorderlessWindow(IDXGISwapChain* swapChain) {
  if (!borderlessWindow())
    return;
  HWND window = g_window.load(std::memory_order_relaxed);
  if (!window || !IsWindow(window))
    return;
  restyle(window, !g_applied.exchange(true, std::memory_order_relaxed));
  if (!swapChain)
    return;

  // Borderless and exclusive fullscreen are alternatives, so take the built-in
  // one out of play entirely. Without this, DXGI still owns alt-enter and would
  // hand the display back to exclusive fullscreen behind our back, leaving a
  // window that is styled borderless but no longer behaving like one.
  BOOL fullscreen = FALSE;
  if (SUCCEEDED(swapChain->GetFullscreenState(&fullscreen, nullptr)) &&
      fullscreen) {
    swapChain->SetFullscreenState(FALSE, nullptr);
    static std::atomic<bool> reported{false};
    if (verboseLogging() ||
        !reported.exchange(true, std::memory_order_relaxed))
      log("Borderless: swap chain pulled back out of exclusive fullscreen");
  }
  IDXGIFactory* factory = nullptr;
  if (SUCCEEDED(swapChain->GetParent(IID_IDXGIFactory,
        reinterpret_cast<void**>(&factory))) && factory) {
    // DXGI_MWA_NO_ALT_ENTER only stops DXGI's own handler; the game may still
    // implement its own toggle, which the per-frame restyle then corrects.
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();
  }
}

void maintainBorderlessWindow() {
  if (!borderlessWindow())
    return;
  // Once a second rather than every frame. The check itself is cheap, but the
  // restyle it can trigger is not something to risk at frame rate, and nothing
  // that reverts the window needs correcting within 16ms.
  static std::atomic<uint32_t> tick { 0 };
  if (tick.fetch_add(1, std::memory_order_relaxed) % 60 != 0)
    return;
  HWND window = g_window.load(std::memory_order_relaxed);
  if (!window || !IsWindow(window))
    return;
  restyle(window, false);
}

}  // namespace atfix
