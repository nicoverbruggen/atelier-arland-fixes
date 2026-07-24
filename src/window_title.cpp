// SPDX-License-Identifier: MIT
//
// Window-title fix for the multilingual Arland executables.
//
// The Japanese/Chinese build passes a UTF-8-encoded title to the ANSI window API
// (CreateWindowExA). On a system whose ANSI codepage is not UTF-8 (1252 under
// Wine/Proton, and legacy Western/Japanese Windows) that title bar is garbage.
//
// We cannot make it show the real Japanese: the game's window is ANSI-classed
// (IsWindowUnicode == 0), so every title, even one set via SetWindowTextW, is
// marshalled through the system ANSI codepage at the window-proc boundary. On a
// non-UTF-8 codepage the Japanese characters have no mapping and collapse to '?'
// (confirmed by reading the title back after SetWindowTextW: it stored as 003F).
// That happens on Windows too, not just Wine, so decoding the title is a dead end.
//
// What DOES survive that round-trip is plain ASCII. So on a non-UTF-8 codepage we
// replace the unshowable title with a readable ASCII transliteration of it. When
// the ANSI codepage IS UTF-8 the real title already displays, so we stand down.
//
// Gating: only the multilingual build, only when the game language is Japanese,
// and only on a non-Japanese, non-UTF-8 ANSI codepage (where the Japanese title
// cannot render). The substitute is chosen per game from the detected title.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

#include "game.h"       // atfix::currentTitle / Title
#include "hook_util.h"  // atfix::installMinHookDetour
#include "log.h"
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

// The multilingual (Japanese and Chinese) executables are the ones without the
// "_en"/"_EN" suffix the English builds carry (see the game table in
// menu_fix.cpp: A11R/A12V/A13V "_x64_Release.exe" vs "..._Release_en.exe").
bool isMultilingualArlandExe() {
  if (currentTitle() == Title::Unknown)
    return false;
  char path[MAX_PATH] = {};
  HMODULE module = GetModuleHandleW(nullptr);
  if (!module || !GetModuleFileNameA(module, path, sizeof(path)))
    return false;
  const char* name = baseName(path);
  const size_t len = std::strlen(name);
  // Reject "...._en.exe" / "...._EN.exe" (7 chars incl. the ".exe").
  const char* enSuffix = "_en.exe";
  const size_t suffixLen = 7;
  if (len >= suffixLen && !_strnicmp(name + len - suffixLen, enSuffix, suffixLen))
    return false;
  return true;
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
  char* back = std::strrchr(path, '\\');
  char* forward = std::strrchr(path, '/');
  char* sep = back > forward ? back : forward;
  if (!sep)
    return false;
  const char* settings = "ArlandDX_Settings.ini";
  if (static_cast<size_t>(sep + 1 - path) + std::strlen(settings) + 1 >
      sizeof(path))
    return false;
  std::strcpy(sep + 1, settings);
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

  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (!user32)
    user32 = LoadLibraryW(L"user32.dll");
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

  log("Window-title ASCII substitution ", any ? "installed" : "failed",
    " (system codepage ", static_cast<unsigned>(acp), ")");
}

}  // namespace atfix
