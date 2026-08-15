// SPDX-License-Identifier: MIT
//
// Implementation. What this fixes and why it takes this shape is in
// window_title.h; what is here is the per-build wiring and the notes that
// only mean anything beside the code they sit on.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

#include "../../core/game.h"       // atfix::currentTitle / Title
#include "../../core/hook_util.h"  // atfix::installMinHookDetour
#include "../../core/log.h"
#include "../../core/path_util.h"
#include "window_title.h"

namespace atfix {

extern Log log;   // lives in main.cpp

namespace {

using PFN_CreateWindowExA = HWND (WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int,
  int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
using PFN_SetWindowTextA = BOOL (WINAPI*)(HWND, LPCSTR);

PFN_CreateWindowExA originalCreateWindowExA = nullptr;
PFN_SetWindowTextA originalSetWindowTextA = nullptr;

const char* baseName(const char* path) {
  const char* back = std::strrchr(path, '\\');
  const char* forward = std::strrchr(path, '/');
  const char* sep = back > forward ? back : forward;
  return sep ? sep + 1 : path;
}

// Match the three supported multilingual executable names exactly. currentTitle
// intentionally recognizes a wider prefix for feature selection, but installing
// system-API hooks uses the same exact-name discipline as game-code hooks.
bool isMultilingualArlandExe() {
  if (currentTitle() == Title::Unknown)
    return false;
  char path[MAX_PATH] = {};
  HMODULE module = GetModuleHandleW(nullptr);
  if (!module || !GetModuleFileNameA(module, path, sizeof(path)))
    return false;
  const char* name = baseName(path);
  return !_stricmp(name, "A11R_x64_Release.exe") ||
    !_stricmp(name, "A12V_x64_Release.exe") ||
    !_stricmp(name, "A13V_x64_Release.exe");
}

// Plain-ASCII transliteration of each game's full Japanese title, e.g. Rorona's
// "ロロナのアトリエ ～アーランドの錬金術士～ DX", read "Rorona no Atelier ~Arland
// no Renkinjutsushi~ DX". ASCII survives the ANSI-window codepage round-trip, so
// this is what the title bar can actually display. (Swap to the localized English
// names, "Atelier Rorona ~The Alchemist of Arland~ DX" etc., if preferred.)
const char* asciiTitleForGame() {
  switch (currentTitle()) {
    case Title::Rorona: return "Rorona no Atelier ~Arland no Renkinjutsushi~ DX";
    case Title::Totori: return "Totori no Atelier ~Arland no Renkinjutsushi 2~ DX";
    case Title::Meruru: return "Meruru no Atelier ~Arland no Renkinjutsushi 3~ DX";
    default:            return nullptr;
  }
}

// Whether the game is configured for Japanese ([Lang] Language == 1 in
// ArlandDX_Settings.ini beside the exe; 2 = English, 3/4 = Chinese). We only
// transliterate the Japanese title, so the Chinese title and an English-language
// run are left untouched.
bool gameLanguageIsJapanese() {
  char path[MAX_PATH] = {};
  HMODULE module = GetModuleHandleW(nullptr);
  if (!module || !GetModuleFileNameA(module, path, sizeof(path)))
    return false;
  if (!replaceFileName(path, sizeof(path), "ArlandDX_Settings.ini"))
    return false;
  return GetPrivateProfileIntA("Lang", "Language", 0, path) == 1;
}

bool isNonAscii(const char* text) {
  if (!text)
    return false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
       *p; ++p) {
    if (*p >= 0x80)
      return true;
  }
  return false;
}

void setTitleA(HWND window, const char* text) {
  if (originalSetWindowTextA)
    originalSetWindowTextA(window, text);
  else
    SetWindowTextA(window, text);
}

// When `text` is the game's non-ASCII (UTF-8 Japanese) title, replace it with the
// per-game ASCII transliteration. Returns true if it set the substitute (so the
// caller should not also apply the original); false leaves the title untouched,
// so plain-ASCII titles and non-Arland games pass through unchanged.
bool applyAsciiTitle(HWND window, const char* text) {
  if (!window || !isNonAscii(text))
    return false;
  // Top-level windows only. SetWindowTextA sets the text of any window it is
  // given, child controls included, and the match above is nothing narrower
  // than "holds a byte >= 0x80" -- so without this a control's caption could be
  // replaced by the game's title. The game's own window has no parent.
  if (GetParent(window))
    return false;
  // And only windows the executable itself created: these detours are
  // process-wide, so another injected DLL's top-level window with a non-ASCII
  // caption would otherwise be renamed too. The game's window qualifies: all
  // three multilingual builds pass their own image base as the instance to
  // CreateWindowExA (lea rcx, [__ImageBase] at every call site into the
  // window-create routine), the same value the window-background fix already
  // matches at class registration.
  if (reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE)) !=
      GetModuleHandleW(nullptr))
    return false;
  const char* ascii = asciiTitleForGame();
  if (!ascii)
    return false;
  setTitleA(window, ascii);
  return true;
}

HWND WINAPI hookedCreateWindowExA(DWORD exStyle, LPCSTR className,
    LPCSTR windowName, DWORD style, int x, int y, int width, int height,
    HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
  HWND window = originalCreateWindowExA(exStyle, className, windowName, style,
    x, y, width, height, parent, menu, instance, param);
  if (window)
    applyAsciiTitle(window, windowName);  // no-op for ASCII titles
  return window;
}

BOOL WINAPI hookedSetWindowTextA(HWND window, LPCSTR text) {
  // Substituting via setTitleA (the un-hooked original) does not re-enter this
  // hook. ASCII titles fall through to the original unchanged.
  if (applyAsciiTitle(window, text))
    return TRUE;
  return originalSetWindowTextA(window, text);
}

}  // namespace

void installWindowTitleFix() {
  static bool attempted = false;
  if (attempted)
    return;
  attempted = true;

  if (!isMultilingualArlandExe())
    return;

  // Substitute only the Japanese title, and only on a locale that cannot show it.
  // On a Japanese ANSI codepage (932) or a UTF-8 codepage (65001) the real title
  // renders, so stand down; a non-Japanese game language (English/Chinese) is not
  // touched either. That leaves the 1252-under-Wine/Proton case this fixes.
  const UINT acp = GetACP();
  if (acp == 932 || acp == CP_UTF8)
    return;
  if (!gameLanguageIsJapanese())
    return;

  // user32 is a static import of this DLL, so the loader has mapped it before
  // this runs and the lookup cannot fail. No LoadLibrary fallback: this is
  // called from DllMain, where LoadLibrary is forbidden.
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (!user32) {
    log("Window-title fix: user32.dll unavailable");
    return;
  }

  auto* createWindowExA = reinterpret_cast<BYTE*>(
    GetProcAddress(user32, "CreateWindowExA"));
  auto* setWindowTextA = reinterpret_cast<BYTE*>(
    GetProcAddress(user32, "SetWindowTextA"));

  bool any = false;
  if (createWindowExA && installMinHookDetour(createWindowExA,
      reinterpret_cast<void*>(&hookedCreateWindowExA),
      reinterpret_cast<void**>(&originalCreateWindowExA)))
    any = true;
  if (setWindowTextA && installMinHookDetour(setWindowTextA,
      reinterpret_cast<void*>(&hookedSetWindowTextA),
      reinterpret_cast<void**>(&originalSetWindowTextA)))
    any = true;

  log("FIXES window_title=", any ? "active" : "failed",
    " system_codepage=", static_cast<unsigned>(acp));
}

}  // namespace atfix
