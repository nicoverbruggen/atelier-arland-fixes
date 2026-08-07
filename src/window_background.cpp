// SPDX-License-Identifier: MIT
//
// Grey-flash fix for the Arland window.
//
// All six executables register their window class with GRAY_BRUSH as the class
// background:
//
//     xor  ecx, ecx            ; r13 = 0 throughout the registration
//     lea  ecx, [r13 + 2]      ; 2 = GRAY_BRUSH
//     call GetStockObject
//     mov  qword ptr [rbp - 0x50], rax   ; WNDCLASSEXA::hbrBackground
//
// The window procedure is short and forwards everything it does not special
// case to DefWindowProcA, so WM_ERASEBKGND is answered by filling the client
// area with that brush. Between the window appearing and the first Present
// there is therefore a mid-grey (128,128,128) rectangle, which is the grey
// screen at startup. It lasts as long as device creation and the first frame's
// work, roughly a second, and it is the game's own doing rather than anything
// Wine or Proton adds; Windows shows it too.
//
// Black is what the game fades up from, so the flash disappears into the
// intro instead of announcing itself. Substituting the brush at registration
// is enough: nothing else reads hbrBackground, and the class is registered
// once.
//
// This hooks RegisterClassExA rather than patching the six call sites because
// the substitution needs no addresses and no prologue gating. It runs from
// DLL_PROCESS_ATTACH, before the game's entry point, so it is in place by the
// time the class is registered.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "game.h"       // atfix::currentTitle / Title
#include "hook_util.h"  // atfix::installMinHookDetour
#include "log.h"
#include "window_background.h"

namespace atfix {

extern Log log;   // lives in main.cpp

namespace {

using PFN_RegisterClassExA = ATOM (WINAPI*)(const WNDCLASSEXA*);

PFN_RegisterClassExA originalRegisterClassExA = nullptr;

// The engine's window class, the same string in all six executables.
constexpr char kEngineClassName[] = "KTGL.A11";

bool isEngineClass(const WNDCLASSEXA* wc) {
  // A class name can be an atom rather than a pointer; those are never ours.
  if (!wc->lpszClassName || IS_INTRESOURCE(wc->lpszClassName))
    return false;
  return !lstrcmpA(wc->lpszClassName, kEngineClassName);
}

ATOM WINAPI hookedRegisterClassExA(const WNDCLASSEXA* wc) {
  if (!wc)
    return originalRegisterClassExA(wc);

  // Three conditions have to hold together: the class comes from the executable
  // itself rather than from an injected DLL, it carries our engine's class name,
  // and its background is the grey stock brush specifically. Stock-object
  // handles are process-wide constants, so that comparison is exact. Anything
  // else registers unchanged, which also means this quietly stands down if a
  // build ever stops doing it. ReShade gates the same API the same way on
  // hInstance (source/windows/user32.cpp).
  if (wc->hInstance != GetModuleHandleW(nullptr) || !isEngineClass(wc) ||
      wc->hbrBackground != static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)))
    return originalRegisterClassExA(wc);

  WNDCLASSEXA substitute = *wc;
  substitute.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  const ATOM atom = originalRegisterClassExA(&substitute);
  log("FIXES window_background=black class=", wc->lpszClassName,
    " atom=", static_cast<unsigned>(atom));
  return atom;
}

}  // namespace

void installWindowBackgroundFix() {
  static bool attempted = false;
  if (attempted)
    return;
  attempted = true;

  if (currentTitle() == Title::Unknown)
    return;

  // user32 is a static import of this DLL, so the loader has mapped it before
  // this runs and the lookup cannot fail. No LoadLibrary fallback: this is
  // called from DllMain, where LoadLibrary is forbidden.
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (!user32) {
    log("Window-background fix: user32.dll unavailable");
    return;
  }

  auto* registerClassExA = reinterpret_cast<BYTE*>(
    GetProcAddress(user32, "RegisterClassExA"));
  if (!registerClassExA)
    return;

  // Nothing is logged here on success. The class has not been registered yet,
  // so the interesting line is the one the hook writes when it substitutes.
  installMinHookDetour(registerClassExA,
    reinterpret_cast<void*>(&hookedRegisterClassExA),
    reinterpret_cast<void**>(&originalRegisterClassExA));
}

}  // namespace atfix
