// SPDX-License-Identifier: MIT
//
// arland-fix-launcher.exe: the mod's launcher. It edits every setting the mod
// has and starts the game, and it is what msimg32.dll opens in place of Koei
// Tecmo's own launcher when the game is started from Steam.
//
// It reads and writes the same keys the DLL parses in src/config.cpp (and SMAA
// in smaa.cpp, AnisotropicFiltering in sync_fix.cpp), using the exact same
// GetPrivateProfileStringA / WritePrivateProfileStringA API so the on-disk
// format matches the mod. On save it touches only the known keys, so any other
// or legacy keys already in the file are preserved. It also writes the game's
// own ArlandDX_Settings.ini where a setting spans both.
//
// It configures whichever game folder it is run from, which is not always the
// folder it lives in: see resolveGameFolder.
//
// No external dependencies: plain Win32 common controls, GUI subsystem. The
// modern look is the ComCtl32 v6 manifest in arland-fix-launcher.rc plus the
// system UI font; there is no toolkit here.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <vector>

#include "../embedded_font.h"

namespace {

// Control identifiers.
enum : int {
  IDC_FONT = 1001,
  IDC_BASE,     // base (display) resolution dropdown
  IDC_SS,       // supersampling multiplier dropdown
  IDC_RENDLBL,  // read-only computed render-resolution label
  IDC_MSAA,
  IDC_SHADOW,
  IDC_ANISO,
  IDC_SMAA,
  IDC_PRESET,
  IDC_TABS,
  IDC_WINMODE,
  IDC_LANG,
  IDC_BSHADOW,
  IDC_BCUTINSHADOW,
  IDC_BCUTINDIM,
  IDC_VERBOSE,
  IDC_START,
  IDC_OPENENV,       // Koei Tecmo's own settings editor
  IDC_OPENLAUNCHER,  // Koei Tecmo's own launcher
  IDC_PLAYVANILLA,   // the game with the mod stood down
  IDC_RESET,
  IDC_CLOSE,
  IDC_REPOLINK,
};

// Absolute path to the arland-fix.ini we edit. Resolved at startup, changeable
// via Browse.
char g_iniPath[MAX_PATH] = {};        // arland-fix.ini, beside this exe
char g_settingsPath[MAX_PATH] = {};   // ArlandDX_Settings.ini, same folder
char g_gameExePath[MAX_PATH] = {};    // an installed game exe, for the icon
char g_gameDir[MAX_PATH] = {};        // its folder, used as the working directory
const wchar_t* g_gameName = nullptr;  // null when no game was recognised

// The games the mod supports, matched in the folder this tool sits in.
//
// Each ships as two executables and the language decides which one runs: the
// `_en` build is the English one, and the other is the multilingual build that
// carries Japanese, Simplified Chinese and Traditional Chinese. Both are
// normally installed side by side, so which one is present is not the question
// -- which one to start is (see gameExeForLanguage).
struct Game {
  const char* english;
  const char* multilingual;
  const wchar_t* name;
};
const Game kGames[] = {
  { "A11R_x64_Release_en.exe", "A11R_x64_Release.exe", L"Atelier Rorona DX" },
  { "A12V_x64_Release_en.exe", "A12V_x64_Release.exe", L"Atelier Totori DX" },
  { "A13V_x64_Release_EN.exe", "A13V_x64_Release.exe", L"Atelier Meruru DX" },
};
const int kGameCount = 3;
int g_game = -1;   // index into kGames, -1 when the folder holds no game

// Handles to the controls we read from and write to.
HWND g_hTabs = nullptr;
HWND g_hDesc[32] = {};   // greyed notes; drawn in kSecondaryText
int  g_descCount = 0;
HWND g_pageCtrls[4][40] = {};   // which controls belong to which tab page
int  g_pageCount[4] = {};
HWND g_hStart = nullptr;   // focused at startup; see createControls
HWND g_hGameLabel = nullptr;   // sits on the tab strip; painted transparent
HWND g_hRepoLink = nullptr;    // SysLink at the bottom of the Display page

// Shown in full and opened on click, so it is the one string in this window
// that has to be right: it is where the window sends people.
const wchar_t* const kRepositoryUrl =
  L"https://github.com/nicoverbruggen/atelier-arland-fixes";
HWND g_hPreset, g_hWinMode, g_hLang,
     g_hFont, g_hBase, g_hSS, g_hRendLbl, g_hMsaa, g_hShadow,
     g_hAniso, g_hSmaa, g_hBShadow, g_hBCutInShadow, g_hBCutInDim, g_hVerbose;

HFONT g_uiFont = nullptr;

// The window paints itself white rather than taking the system's colours.
//
// Windows draws dialogs on COLOR_BTNFACE, a light grey, while a themed tab page
// is white -- so a window that mixes the two has a grey surround and a white
// panel, and every label has to be told which one it is standing on. Wine
// happens to render the dialog face white, so the same build looked coherent
// there and mismatched on Windows. Choosing white everywhere makes the two
// platforms agree and removes the distinction entirely.
//
// The text colours are fixed for the same reason: with the background pinned,
// a system text colour from a dark scheme would be white on white.
const COLORREF kBackground = RGB(255, 255, 255);
const COLORREF kText = RGB(0, 0, 0);
const COLORREF kSecondaryText = RGB(102, 102, 102);
HBRUSH g_backgroundBrush = nullptr;

// Every coordinate in this file is a logical unit at 100%, and the control
// helpers scale on the way to CreateWindow -- so the layout is written once and
// the scale is decided once, rather than each literal having to know about it.
//
// The base layout is 484x548, chosen to fit a 1280x720 screen with its frame
// and a taskbar, because that is the smallest screen this has to work on: a
// 720p TV, a handheld, or Big Picture on either. Scaling up (for a controller
// at TV distance) is therefore always optional and always clamped to the
// monitor's work area, so no scale can push the buttons off-screen.
const int kBaseWidth = 700;
const int kBaseHeight = 440;

// Two separate factors, because they do not apply to the same things.
//
// g_dpiScale is the display's own scaling. The process is DPI aware, so the
// system metrics we ask for -- notably the UI font in createUiFont -- ALREADY
// come back at that scale. Only the coordinates in this file, which are written
// at 100%, still need it applied.
//
// g_userScale is our own enlargement, for a controller at TV distance. Nothing
// else knows about it, so it applies to the layout AND to the font.
//
// Multiplying the font by both is the mistake this split exists to prevent: it
// would scale the DPI contribution twice and give giant text in a window sized
// for normal text.
int g_dpiScale = 100;    // percent, from the display
int g_userScale = 100;   // percent, ours

int S(int value) { return value * g_dpiScale / 100 * g_userScale / 100; }

// The dropdown entries. The first column is the label shown to the user, the
// second is the exact string written to the ini.
struct ComboItem { const wchar_t* label; const char* value; };

// The second column is the exact string written to the ini and must not change;
// only the labels are worded for the person reading them.
const ComboItem kFontItems[] = {
  { L"High quality alternatives",  "replaced" },
  { L"Original upscaled",                   "upscaled" },
  { L"Original",                            "original" },
};
// MSAA / ShadowMultiplier share the same 1/2/4/8 scale (1 = off). The labels
// name what the number means, since the group labels above them are written in
// plain language and a bare "8" would say nothing on its own. The second column
// is the exact string written to the ini and must not change.
const ComboItem kMsaaItems[] = {
  { L"Off",                  "1" }, { L"2x samples",  "2" },
  { L"4x samples",           "4" }, { L"8x samples",  "8" },
};
const ComboItem kShadowItems[] = {
  { L"Normal (1024 map)",    "1" }, { L"2x (2048 map)", "2" },
  { L"4x (4096 map)",        "4" }, { L"8x (8192 map)", "8" },
};
const ComboItem kAnisoItems[] = {
  { L"Off",                  "1" }, { L"2x anisotropic",  "2" },
  { L"4x anisotropic",       "4" }, { L"8x anisotropic",  "8" },
  { L"16x anisotropic",     "16" },
};

// Base (display / backbuffer) resolutions. w == 0 means "Auto" (blank in the
// ini, keeping the launcher's resolution). 16:9 is fine for these games.
struct ResItem { const wchar_t* label; unsigned w, h; };
const ResItem kBaseItems[] = {
  { L"Auto (launcher resolution)", 0,    0    },
  { L"1280 x 720",                 1280, 720  },
  { L"1920 x 1080",                1920, 1080 },
  { L"2560 x 1440",                2560, 1440 },
  { L"3840 x 2160",                3840, 2160 },
};
const int kBaseCount = 5;

// The largest mode the display reports. The base resolution is what gets
// presented, so offering more than the panel has is offering a worse picture:
// the extra pixels are only scaled away again. Rendering higher than the screen
// is what the supersampling multiplier below is for.
void displayMaximum(unsigned* w, unsigned* h) {
  *w = 0;
  *h = 0;
  DEVMODEW mode = { };
  mode.dmSize = sizeof(mode);
  for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &mode); ++i) {
    if ((unsigned long long)mode.dmPelsWidth * mode.dmPelsHeight >
        (unsigned long long)*w * *h) {
      *w = mode.dmPelsWidth;
      *h = mode.dmPelsHeight;
    }
  }
  if (!*w || !*h) {
    const int cx = GetSystemMetrics(SM_CXSCREEN);
    const int cy = GetSystemMetrics(SM_CYSCREEN);
    if (cx > 0 && cy > 0) { *w = (unsigned)cx; *h = (unsigned)cy; }
  }
}

// Supersampling per-axis multipliers. Index 0 is "Off" (no supersampling);
// its 1.0 factor is never applied because Off writes the Render keys blank.
struct MultItem { const wchar_t* label; double mult; };
const MultItem kSSItems[] = {
  { L"Off",   1.0 },  { L"1.25x", 1.25 }, { L"1.5x", 1.5 },
  { L"2x",    2.0 },  { L"3x",    3.0  }, { L"4x",   4.0 },
};
const int kSSCount = 6;

// The ceiling on the internal render resolution: 8K, four times the pixels of
// 4K. Past this there is nothing left to resolve -- these are 2010-era assets,
// and the sub-pixel detail that supersampling recovers is exhausted well before
// 8K -- while the cost keeps scaling with pixels drawn: a 4K panel at 4x is
// 15360x8640, over half a gigabyte for a single render target, and the games
// allocate many. Multipliers that would exceed it are not offered (see
// refillSupersampling), and the mod clamps the ini as well.
//
// This could be raised if a reason to ever appears. It is a judgement about
// what these games can use, not a limit of the implementation.
const unsigned kMaxRenderWidth = 7680;
const unsigned kMaxRenderHeight = 4320;

// Quality presets. These set only the Image Quality group -- resolution,
// borderless, frame rate, the battle options and the UI font are preferences
// rather than quality levels, and a preset that silently changed them would
// surprise more than it helped. Selecting Custom changes nothing; any manual
// edit switches the box to Custom so it never claims a preset it is not on.
struct Preset {
  const wchar_t* label;
  int supersampling;   // index into kSSItems
  int msaa;            // index into kMsaaItems
  bool smaa;
  int aniso;           // index into kAnisoItems
  int shadow;          // index into kShadowItems
};
// The ladder spends in the order things are worth paying for: edge smoothing
// first (SMAA is nearly free), then multisampling, then anisotropic filtering
// and shadow maps, and only at the top supersampling, which costs the most by
// a wide margin.
//
// Balanced matches what the mod ships with (default.ini and the literals in
// src/), so a fresh install opens on a named preset rather than on Custom, and
// the launcher and the drop-in DLL agree about what "default" means.
//
// It leans on SMAA rather than MSAA for anti-aliasing: SMAA is on at every tier
// and costs one cheap full-frame pass, where multisampling costs memory and
// bandwidth on every render target. So MSAA starts at Medium, for people who
// want more than the shader gives, and the default spends nothing on it.
const Preset kPresets[] = {
  //                        SS  MSAA  SMAA  aniso  shadow
  { L"Low",                  0,   0,  true,     0,      0 },
  { L"Balanced (default)",   0,   0,  true,     3,      0 },
  { L"Medium",               0,   2,  true,     3,      0 },
  { L"High",                 2,   0,  true,     4,      1 },
  { L"Maximum",              3,   0,  true,     4,      2 },
  { L"Custom",              -1,  -1,  true,    -1,     -1 },
};
const int kPresetCount = 6;
const int kPresetCustom = 5;

// Window mode spans two files: Borderless in arland-fix.ini, FullScreen in the
// game's own ArlandDX_Settings.ini. Borderless is a window as far as the game
// is concerned, so it writes FullScreen=0 -- that way disabling the mod leaves
// a sane windowed game rather than an unexpected mode change.
struct WindowModeItem { const wchar_t* label; bool borderless; const char* fullscreen; };
const WindowModeItem kWindowModes[] = {
  { L"Windowed",              false, "0" },
  { L"Borderless Fullscreen", true,  "0" },
  { L"Fullscreen",            false, "1" },
};
const int kWindowModeCount = 3;

// The game's own [Lang] Language value. Which of these an executable honours
// depends on the build, so all four are offered and the game decides.
const ComboItem kLangItems[] = {
  { L"Japanese",              "1" },
  { L"English",               "2" },
  { L"Chinese (Simplified)",  "3" },
  { L"Chinese (Traditional)", "4" },
};
const int kLangCount = 4;

// Pack a base resolution into a combo item's LPARAM (w and h each fit in the
// 640..16384 range, so 16 bits apiece). 0 == Auto.
LPARAM packRes(unsigned w, unsigned h) {
  return (LPARAM)(((unsigned long long)w << 16) | h);
}

// ---- ini helpers -----------------------------------------------------------

// Read a string value; returns true and fills out when the key is present.
bool iniString(const char* section, const char* key, char* out, DWORD outSize) {
  // Use a sentinel default so we can tell "absent" from "present but empty".
  GetPrivateProfileStringA(section, key, "\x01", out, outSize, g_iniPath);
  if (out[0] == '\x01') { out[0] = '\0'; return false; }
  return true;
}

// Read a boolean the same way the mod does: first character t/T/1/y/Y is true.
bool iniBool(const char* section, const char* key, bool def) {
  char value[16] = {};
  if (!iniString(section, key, value, sizeof(value)))
    return def;
  return value[0] == 't' || value[0] == 'T' || value[0] == '1' ||
         value[0] == 'y' || value[0] == 'Y';
}

void iniWriteBool(const char* section, const char* key, bool on) {
  WritePrivateProfileStringA(section, key, on ? "true" : "false", g_iniPath);
}

// ---- combo helpers ---------------------------------------------------------

void comboFill(HWND combo, const ComboItem* items, int count) {
  for (int i = 0; i < count; ++i)
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)items[i].label);
}

// Select the entry whose ini value matches raw (case-insensitive prefix so the
// mod's aliases like "off" -> original and "upscale..." -> upscaled resolve).
void comboSelectByValue(HWND combo, const ComboItem* items, int count,
                        const char* raw, int fallback) {
  int pick = fallback;
  for (int i = 0; i < count; ++i) {
    if (_stricmp(raw, items[i].value) == 0) { pick = i; break; }
  }
  SendMessageW(combo, CB_SETCURSEL, pick, 0);
}

const char* comboValue(HWND combo, const ComboItem* items, int count) {
  int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
  if (sel < 0 || sel >= count) sel = 0;
  return items[sel].value;
}

// Map the mod's Font parsing (config.cpp uiFontMode) onto a combo index.
int fontIndexFromRaw(const char* raw) {
  if (_strnicmp(raw, "original", 8) == 0 || _strnicmp(raw, "off", 3) == 0)
    return 2;
  if (_strnicmp(raw, "upscale", 7) == 0)
    return 1;
  return 0;  // replaced (the default)
}

// ---- base resolution / supersampling ---------------------------------------

// The currently selected base resolution. Returns false for "Auto" (no
// concrete size), in which case supersampling cannot be computed.
bool selectedBase(unsigned* w, unsigned* h) {
  int sel = (int)SendMessageW(g_hBase, CB_GETCURSEL, 0, 0);
  if (sel < 0) return false;
  LPARAM d = (LPARAM)SendMessageW(g_hBase, CB_GETITEMDATA, sel, 0);
  unsigned bw = (unsigned)((unsigned long long)d >> 16);
  unsigned bh = (unsigned)(d & 0xFFFF);
  if (!bw || !bh) return false;   // Auto
  *w = bw; *h = bh;
  return true;
}

// The render resolution a multiplier produces over a base, and whether it is
// within the 8K ceiling. Both are needed: one to show, one to decide whether to
// offer the multiplier at all.
void renderFor(unsigned bw, unsigned bh, double mult,
               unsigned* rw, unsigned* rh) {
  *rw = (unsigned)(bw * mult + 0.5);
  *rh = (unsigned)(bh * mult + 0.5);
}

bool withinRenderLimit(unsigned bw, unsigned bh, double mult) {
  unsigned rw, rh;
  renderFor(bw, bh, mult, &rw, &rh);
  return rw <= kMaxRenderWidth && rh <= kMaxRenderHeight;
}

// The supersampling dropdown holds only the multipliers that fit the current
// base, so its indices are not kSSItems indices; each item carries its own as
// item data. These two are the only way to read and write the selection.
int ssIndex() {
  const int sel = (int)SendMessageW(g_hSS, CB_GETCURSEL, 0, 0);
  if (sel < 0) return 0;
  const int index = (int)SendMessageW(g_hSS, CB_GETITEMDATA, sel, 0);
  return index >= 0 && index < kSSCount ? index : 0;
}

// Select the entry for a kSSItems index, or Off when the list does not hold it
// (the base is too large for that multiplier).
void setSsIndex(int index) {
  const int count = (int)SendMessageW(g_hSS, CB_GETCOUNT, 0, 0);
  for (int i = 0; i < count; ++i) {
    if ((int)SendMessageW(g_hSS, CB_GETITEMDATA, i, 0) == index) {
      SendMessageW(g_hSS, CB_SETCURSEL, i, 0);
      return;
    }
  }
  SendMessageW(g_hSS, CB_SETCURSEL, 0, 0);
}

// The selected supersampling multiplier, or 0.0 when "Off" (index 0).
double selectedMult() {
  const int index = ssIndex();
  if (index <= 0 || index >= kSSCount) return 0.0;
  return kSSItems[index].mult;
}

// Rebuild the multiplier list for the current base, keeping the selection if it
// still fits. Anything that would render above 8K is left out rather than
// offered and then silently clamped: a 4K panel can have 1.5x but not 3x, and
// the difference matters enough to be visible in the list.
// Set when the multiplier that was selected (or loaded from the ini) does not
// fit the current base and was reduced to the largest one that does. Someone
// who asked for 4x at 4K wants as much supersampling as they can have, not
// none of it, so the cap lands them on 2x -- and it says so, because silently
// changing a setting and then writing it back on Save is how a configuration
// gets lost.
bool g_ssReduced = false;

void refillSupersampling() {
  const int wanted = ssIndex();
  unsigned bw = 0, bh = 0;
  const bool haveBase = selectedBase(&bw, &bh);
  SendMessageW(g_hSS, CB_RESETCONTENT, 0, 0);
  for (int i = 0; i < kSSCount; ++i) {
    if (i && haveBase && !withinRenderLimit(bw, bh, kSSItems[i].mult))
      continue;
    // Each multiplier carries the resolution it produces, so the answer to
    // "what does 2x actually render at?" is in the list being chosen from
    // rather than somewhere else in the window. Off has no resolution of its
    // own, and with an Auto base there is nothing to compute against.
    wchar_t label[96];
    if (i && haveBase) {
      unsigned rw = 0, rh = 0;
      renderFor(bw, bh, kSSItems[i].mult, &rw, &rh);
      wsprintfW(label, L"%s  (%u x %u)", kSSItems[i].label, rw, rh);
    } else {
      lstrcpynW(label, kSSItems[i].label, 96);
    }
    const int at = (int)SendMessageW(g_hSS, CB_ADDSTRING, 0, (LPARAM)label);
    SendMessageW(g_hSS, CB_SETITEMDATA, at, i);
  }
  setSsIndex(wanted);
  // setSsIndex lands on Off when the list does not hold what was asked for. The
  // list is built in ascending order, so its last entry is the largest
  // multiplier this base allows: take that instead.
  g_ssReduced = false;
  if (wanted > 0 && ssIndex() != wanted) {
    const int count = (int)SendMessageW(g_hSS, CB_GETCOUNT, 0, 0);
    if (count > 1) {
      SendMessageW(g_hSS, CB_SETCURSEL, count - 1, 0);
      g_ssReduced = true;
    }
  }
}

// Compute the render resolution (base x multiplier) into out. Returns false
// when there is nothing to render at (Auto base or Off).
bool computeRender(unsigned* rw, unsigned* rh) {
  unsigned bw, bh;
  double m = selectedMult();
  if (m <= 1.0 || !selectedBase(&bw, &bh))
    return false;
  renderFor(bw, bh, m, rw, rh);
  return true;
}

// Refresh the live render-resolution label and the enabled state of the
// supersampling dropdown. Supersampling needs a concrete base to compute
// against, so it is greyed out and forced to Off while the base is "Auto".
// Push a preset's values into the quality controls.
void applyPreset(int index) {
  if (index < 0 || index >= kPresetCustom)
    return;
  const Preset& preset = kPresets[index];
  setSsIndex(preset.supersampling);
  SendMessageW(g_hMsaa, CB_SETCURSEL, preset.msaa, 0);
  SendMessageW(g_hAniso, CB_SETCURSEL, preset.aniso, 0);
  SendMessageW(g_hShadow, CB_SETCURSEL, preset.shadow, 0);
  SendMessageW(g_hSmaa, BM_SETCHECK,
    preset.smaa ? BST_CHECKED : BST_UNCHECKED, 0);
}

// Which preset the current controls correspond to, or Custom if none.
int detectPreset() {
  for (int i = 0; i < kPresetCustom; ++i) {
    const Preset& preset = kPresets[i];
    if (ssIndex() == preset.supersampling &&
        (int)SendMessageW(g_hMsaa, CB_GETCURSEL, 0, 0) == preset.msaa &&
        (int)SendMessageW(g_hAniso, CB_GETCURSEL, 0, 0) == preset.aniso &&
        (int)SendMessageW(g_hShadow, CB_GETCURSEL, 0, 0) == preset.shadow &&
        (SendMessageW(g_hSmaa, BM_GETCHECK, 0, 0) == BST_CHECKED) == preset.smaa)
      return i;
  }
  return kPresetCustom;
}

void refreshPreset() {
  SendMessageW(g_hPreset, CB_SETCURSEL, detectPreset(), 0);
}

// Defined with the rest of the drawing code below.
void repaintUnder(HWND ctrl);
LRESULT CALLBACK TabProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

void updateRenderResolution() {
  unsigned bw, bh;
  bool haveBase = selectedBase(&bw, &bh);
  refillSupersampling();
  EnableWindow(g_hSS, haveBase);
  if (!haveBase)
    SendMessageW(g_hSS, CB_SETCURSEL, 0, 0);   // force Off

  // The resolutions themselves are in the dropdown now, so this row says only
  // what the dropdown cannot: why the multiplier that was configured is not the
  // one selected. Empty the rest of the time.
  char text[96] = "";
  if (g_ssReduced)
    lstrcpyA(text, "Reduced to fit the 8K limit.");
  else if (!haveBase)
    lstrcpyA(text, "Supersampling needs a fixed resolution above.");
  SetWindowTextA(g_hRendLbl, text);
  // The only label whose text changes while the window is up, so the only one
  // that has to clear what it said before.
  repaintUnder(g_hRendLbl);
}

// ---- load / save -----------------------------------------------------------

void loadFromIni() {
  char buf[32] = {};

  // [Rendering] Font: default "replaced".
  iniString("Rendering", "Font", buf, sizeof(buf));
  SendMessageW(g_hFont, CB_SETCURSEL, fontIndexFromRaw(buf), 0);

  // Base (display) resolution: DisplayWidth/Height, falling back to the legacy
  // Width/Height keys the mod still reads. Blank => Auto. A concrete value that
  // is not one of the listed presets is added as its own item so it round-trips.
  char w[16] = {}, h[16] = {};
  if (!iniString("Rendering", "DisplayWidth", w, sizeof(w)))
    iniString("Rendering", "Width", w, sizeof(w));
  if (!iniString("Rendering", "DisplayHeight", h, sizeof(h)))
    iniString("Rendering", "Height", h, sizeof(h));
  unsigned dispW = (unsigned)std::strtoul(w, nullptr, 10);
  unsigned dispH = (unsigned)std::strtoul(h, nullptr, 10);
  int baseSel = 0;   // Auto by default (index 0)
  if (dispW && dispH) {
    baseSel = -1;
    for (int i = 0; i < kBaseCount; ++i)
      if (kBaseItems[i].w == dispW && kBaseItems[i].h == dispH) { baseSel = i; break; }
    if (baseSel < 0) {   // not a preset: add a custom item so it survives a save
      wchar_t label[32];
      wsprintfW(label, L"%u x %u", dispW, dispH);
      baseSel = (int)SendMessageW(g_hBase, CB_ADDSTRING, 0, (LPARAM)label);
      SendMessageW(g_hBase, CB_SETITEMDATA, baseSel, packRes(dispW, dispH));
    }
  }
  SendMessageW(g_hBase, CB_SETCURSEL, baseSel, 0);

  // Supersampling: infer the multiplier as RenderWidth / DisplayWidth and snap
  // to the nearest listed factor. Blank render, or no concrete base, => Off.
  char rw[16] = {};
  iniString("Rendering", "RenderWidth", rw, sizeof(rw));
  unsigned rendW = (unsigned)std::strtoul(rw, nullptr, 10);
  int ssSel = 0;   // Off
  if (dispW && rendW > dispW) {
    double ratio = (double)rendW / (double)dispW;
    double best = 1e9;
    for (int i = 1; i < kSSCount; ++i) {
      double d = ratio - kSSItems[i].mult;
      if (d < 0) d = -d;
      if (d < best) { best = d; ssSel = i; }
    }
  }
  // The list has to hold the multipliers for this base before one can be
  // selected. A saved value the base no longer allows falls back to Off, which
  // is the same answer the mod's own clamp gives that ini.
  refillSupersampling();
  setSsIndex(ssSel);

  // Sync the computed render-resolution label and the Auto-greys-out rule.
  updateRenderResolution();

  // MSAA / ShadowMultiplier default to 1 (off). Aniso defaults to 8: the mod
  // treats 0 and 1 as off, but an absent key is 8x, not off, because
  // anisotropic filtering ships on (it costs nothing per frame). Falling back
  // to "1" here would show Off for a default install and then persist it.
  iniString("Rendering", "MSAA", buf, sizeof(buf));
  comboSelectByValue(g_hMsaa, kMsaaItems, 4, buf[0] ? buf : "1", 0);
  iniString("Rendering", "ShadowMultiplier", buf, sizeof(buf));
  comboSelectByValue(g_hShadow, kShadowItems, 4, buf[0] ? buf : "1", 0);
  iniString("Rendering", "AnisotropicFiltering", buf, sizeof(buf));
  comboSelectByValue(g_hAniso, kAnisoItems, 5, buf[0] ? buf : "8", 0);

  // SMAA is on by default.
  SendMessageW(g_hSmaa, BM_SETCHECK,
    iniBool("Rendering", "SMAA", true) ? BST_CHECKED : BST_UNCHECKED, 0);

  // Borderless wins when set; otherwise the game's own FullScreen decides.
  const bool borderless = iniBool("Rendering", "Borderless", false);
  const bool fullscreen =
    GetPrivateProfileIntA("Window", "FullScreen", 0, g_settingsPath) != 0;
  SendMessageW(g_hWinMode, CB_SETCURSEL,
    borderless ? 1 : (fullscreen ? 2 : 0), 0);

  // The game's language lives entirely in its own settings file.
  {
    const int lang =
      GetPrivateProfileIntA("Lang", "Language", 2, g_settingsPath);
    int sel = 1;                       // English unless the file says otherwise
    for (int i = 0; i < kLangCount; ++i)
      if (kLangItems[i].value[0] == ('0' + lang))
        sel = i;
    SendMessageW(g_hLang, CB_SETCURSEL, sel, 0);
  }

  // [Battle]: defaults match src/game.cpp (shadows on, cut-in shadows off,
  // cut-in dimming held on).
  SendMessageW(g_hBShadow, BM_SETCHECK,
    iniBool("Battle", "BattleShadows", true) ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageW(g_hBCutInShadow, BM_SETCHECK,
    iniBool("Battle", "BattleCutInShadows", false) ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageW(g_hBCutInDim, BM_SETCHECK,
    iniBool("Battle", "BattleCutInDimming", true) ? BST_CHECKED : BST_UNCHECKED, 0);

  // [Diagnostics].
  SendMessageW(g_hVerbose, BM_SETCHECK,
    iniBool("Diagnostics", "VerboseLogging", false) ? BST_CHECKED : BST_UNCHECKED, 0);

  // Last, once every quality control holds its loaded value: show which preset
  // that combination is, or Custom.
  refreshPreset();
  updateRenderResolution();

}

// Put every setting back to a known-good starting point: the mod's own
// defaults, plus 720p windowed.
//
// Language is the one exception, and deliberately so. It is not tuning that can
// be wrong -- it is what the player reads the game in, and resetting someone to
// English because they wanted their graphics settings back would be a hostile
// reading of "defaults". saveToIni writes it back unchanged from its control.
//
// The values here are the same ones default.ini ships and src/ parses; the
// Balanced preset is defined to match, which is why this leaves the preset box
// reading Balanced rather than Custom.
void resetToDefaults() {
  // 720p windowed: the safe floor. It is the one resolution every display this
  // runs on can show, including a handheld, so a reset from a configuration
  // that does not display correctly always lands somewhere visible.
  const int count = (int)SendMessageW(g_hBase, CB_GETCOUNT, 0, 0);
  int base = 0;   // Auto, if 720p somehow is not in the list
  for (int i = 0; i < count; ++i) {
    if ((LPARAM)SendMessageW(g_hBase, CB_GETITEMDATA, i, 0) ==
        packRes(1280, 720)) {
      base = i;
      break;
    }
  }
  SendMessageW(g_hBase, CB_SETCURSEL, base, 0);
  SendMessageW(g_hWinMode, CB_SETCURSEL, 0, 0);   // Windowed

  SendMessageW(g_hFont, CB_SETCURSEL, 0, 0);      // replaced
  setSsIndex(0);                                  // supersampling off
  SendMessageW(g_hMsaa, CB_SETCURSEL, 0, 0);      // 1x
  SendMessageW(g_hAniso, CB_SETCURSEL, 3, 0);     // 8x, the shipped default
  SendMessageW(g_hShadow, CB_SETCURSEL, 0, 0);    // 1024 map
  SendMessageW(g_hSmaa, BM_SETCHECK, BST_CHECKED, 0);
  SendMessageW(g_hBShadow, BM_SETCHECK, BST_CHECKED, 0);
  SendMessageW(g_hBCutInShadow, BM_SETCHECK, BST_UNCHECKED, 0);
  SendMessageW(g_hBCutInDim, BM_SETCHECK, BST_CHECKED, 0);
  SendMessageW(g_hVerbose, BM_SETCHECK, BST_UNCHECKED, 0);
  refreshPreset();
  updateRenderResolution();
}

bool isChecked(HWND ctrl) {
  return SendMessageW(ctrl, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void saveToIni() {
  // Write only the known keys. WritePrivateProfileStringA leaves every other
  // line in the file untouched, so unknown / legacy keys are preserved.
  WritePrivateProfileStringA("Rendering", "Font",
    comboValue(g_hFont, kFontItems, 3), g_iniPath);

  // Base resolution -> DisplayWidth/DisplayHeight. "Auto" writes them blank
  // (never "0"); an empty string keeps the key present with no value, which the
  // mod reads as "unset". The legacy Width/Height keys are left untouched, so
  // they are preserved (DisplayWidth takes precedence in the mod).
  unsigned bw, bh;
  char num[16] = {};
  if (selectedBase(&bw, &bh)) {
    wsprintfA(num, "%u", bw);
    WritePrivateProfileStringA("Rendering", "DisplayWidth", num, g_iniPath);
    // Keep the game's own settings file in step: the mod overrides the swap
    // chain with the display resolution anyway, and leaving the two disagreeing
    // is what makes "which resolution am I actually running?" hard to answer.
    WritePrivateProfileStringA("Graphics", "ScreenWidth", num, g_settingsPath);
    wsprintfA(num, "%u", bh);
    WritePrivateProfileStringA("Rendering", "DisplayHeight", num, g_iniPath);
    WritePrivateProfileStringA("Graphics", "ScreenHeight", num, g_settingsPath);
  } else {
    WritePrivateProfileStringA("Rendering", "DisplayWidth", "", g_iniPath);
    WritePrivateProfileStringA("Rendering", "DisplayHeight", "", g_iniPath);
  }

  // Supersampling -> RenderWidth/RenderHeight = base x multiplier (clamped to
  // 16384). "Off", or an Auto base, writes them blank.
  unsigned rw, rh;
  if (computeRender(&rw, &rh)) {
    wsprintfA(num, "%u", rw);
    WritePrivateProfileStringA("Rendering", "RenderWidth", num, g_iniPath);
    wsprintfA(num, "%u", rh);
    WritePrivateProfileStringA("Rendering", "RenderHeight", num, g_iniPath);
  } else {
    WritePrivateProfileStringA("Rendering", "RenderWidth", "", g_iniPath);
    WritePrivateProfileStringA("Rendering", "RenderHeight", "", g_iniPath);
  }

  WritePrivateProfileStringA("Rendering", "MSAA",
    comboValue(g_hMsaa, kMsaaItems, 4), g_iniPath);
  WritePrivateProfileStringA("Rendering", "ShadowMultiplier",
    comboValue(g_hShadow, kShadowItems, 4), g_iniPath);
  WritePrivateProfileStringA("Rendering", "AnisotropicFiltering",
    comboValue(g_hAniso, kAnisoItems, 5), g_iniPath);
  iniWriteBool("Rendering", "SMAA", isChecked(g_hSmaa));
  {
    int sel = (int)SendMessageW(g_hWinMode, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= kWindowModeCount)
      sel = 0;
    const WindowModeItem& mode = kWindowModes[sel];
    iniWriteBool("Rendering", "Borderless", mode.borderless);
    WritePrivateProfileStringA("Window", "FullScreen", mode.fullscreen,
      g_settingsPath);
  }
  WritePrivateProfileStringA("Lang", "Language",
    comboValue(g_hLang, kLangItems, kLangCount), g_settingsPath);

  iniWriteBool("Battle", "BattleShadows", isChecked(g_hBShadow));
  iniWriteBool("Battle", "BattleCutInShadows", isChecked(g_hBCutInShadow));
  iniWriteBool("Battle", "BattleCutInDimming", isChecked(g_hBCutInDim));

  iniWriteBool("Diagnostics", "VerboseLogging", isChecked(g_hVerbose));

  // Flush the cache so the file is on disk before we report success.
  WritePrivateProfileStringA(nullptr, nullptr, nullptr, g_iniPath);
  WritePrivateProfileStringA(nullptr, nullptr, nullptr, g_settingsPath);
}

// ---- unsaved-change tracking -----------------------------------------------

// Settings reach the ini when the game is started, not when the window closes,
// so closing after an edit would silently discard it. Rather than watching for
// change notifications, which means remembering every control, this snapshots
// what saveToIni reads and compares on close: no control can be missed, and
// changing a setting back to its old value correctly counts as unchanged.
struct UiState {
  int font, base, ss, msaa, shadow, aniso, winMode, lang;
  int smaa, battleShadows, cutInShadows, cutInDimming, verbose;
};
UiState g_savedState;

UiState currentState() {
  UiState s = {};
  s.font = (int)SendMessageW(g_hFont, CB_GETCURSEL, 0, 0);
  s.base = (int)SendMessageW(g_hBase, CB_GETCURSEL, 0, 0);
  s.ss = ssIndex();
  s.msaa = (int)SendMessageW(g_hMsaa, CB_GETCURSEL, 0, 0);
  s.shadow = (int)SendMessageW(g_hShadow, CB_GETCURSEL, 0, 0);
  s.aniso = (int)SendMessageW(g_hAniso, CB_GETCURSEL, 0, 0);
  s.winMode = (int)SendMessageW(g_hWinMode, CB_GETCURSEL, 0, 0);
  s.lang = (int)SendMessageW(g_hLang, CB_GETCURSEL, 0, 0);
  s.smaa = isChecked(g_hSmaa);
  s.battleShadows = isChecked(g_hBShadow);
  s.cutInShadows = isChecked(g_hBCutInShadow);
  s.cutInDimming = isChecked(g_hBCutInDim);
  s.verbose = isChecked(g_hVerbose);
  return s;
}

// Every member is an int, so there is no padding for memcmp to trip over.
void markSaved() { g_savedState = currentState(); }

bool hasUnsavedChanges() {
  const UiState now = currentState();
  return std::memcmp(&now, &g_savedState, sizeof(now)) != 0;
}

// ---- ini location ----------------------------------------------------------

// Fill g_iniPath with dir + "arland-fix.ini". dir may or may not end in a slash.
void iniPathInDir(const char* dir) {
  lstrcpynA(g_iniPath, dir, MAX_PATH);
  size_t len = std::strlen(g_iniPath);
  if (len && g_iniPath[len - 1] != '\\' && g_iniPath[len - 1] != '/' &&
      len + 1 < MAX_PATH) {
    g_iniPath[len++] = '\\';
    g_iniPath[len] = '\0';
  }
  lstrcpynA(g_iniPath + len, "arland-fix.ini", (int)(MAX_PATH - len));
}

// Join `dir` (which must end in a separator) and `name` into a MAX_PATH `out`.
// False when the result would not fit, leaving out empty: lstrcatA takes no
// bound, so a folder near MAX_PATH would run past the caller's buffer.
bool joinPath(char* out, const char* dir, const char* name) {
  out[0] = '\0';
  const size_t dirLen = std::strlen(dir);
  const size_t nameLen = std::strlen(name);
  if (dirLen + nameLen + 1 > MAX_PATH)
    return false;
  std::memcpy(out, dir, dirLen);
  std::memcpy(out + dirLen, name, nameLen + 1);
  return true;
}

// Join `dir` (which must end in a separator) and `name` into `out`, and say
// whether the result exists.
bool fileInDir(const char* dir, const char* name, char* out) {
  return joinPath(out, dir, name) &&
         GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
}

// Index into kGames of the game installed in `dir`, or -1 if it holds none.
// Either of a game's two executables is enough to recognise it.
int gameInFolder(const char* dir) {
  char candidate[MAX_PATH];
  for (int i = 0; i < kGameCount; ++i) {
    if (fileInDir(dir, kGames[i].english, candidate) ||
        fileInDir(dir, kGames[i].multilingual, candidate))
      return i;
  }
  return -1;
}

// The executable to start for a given [Lang] Language value, written into
// `out`. Koei Tecmo's own launcher runs the `_en` build for Language=2 and the
// multilingual build for 1 (Japanese), 3 (Simplified Chinese) and 4
// (Traditional Chinese); anything else it treats as English. This matches that,
// because the two builds do not each carry every language: starting the English
// one with Language=3 gets English, which is exactly the bug this avoids.
//
// If the build the language calls for is not installed, the other one is used
// rather than refusing to start anything.
bool gameExeForLanguage(const char* language, char* out) {
  if (g_game < 0 || !g_gameDir[0])
    return false;
  const char code = language ? language[0] : '2';
  const bool english = code != '1' && code != '3' && code != '4';
  const Game& game = kGames[g_game];
  const char* first = english ? game.english : game.multilingual;
  const char* second = english ? game.multilingual : game.english;
  return fileInDir(g_gameDir, first, out) ||
         fileInDir(g_gameDir, second, out);
}

// Point every path this tool works with at `dir`. `game` is an index into
// kGames, or -1 when the folder holds no recognised game (the ini path is
// still set, so Save can create one).
void adoptFolder(const char* dir, int game) {
  iniPathInDir(dir);
  // Left empty if the folder sits too deep for the name to fit; such a folder
  // cannot be configured either way, since iniPathInDir truncates as well.
  joinPath(g_settingsPath, dir, "ArlandDX_Settings.ini");
  lstrcpynA(g_gameDir, dir, MAX_PATH);   // keeps its trailing separator
  g_game = game;
  if (game < 0)
    return;
  g_gameName = kGames[game].name;
  // For the icon and for whether Play with mod can do anything at all; which
  // executable actually runs is decided at that point, from the language.
  char language[16] = {};
  GetPrivateProfileStringA("Lang", "Language", "2", language, sizeof(language),
    g_settingsPath);
  if (!gameExeForLanguage(language, g_gameExePath))
    g_gameExePath[0] = '\0';
}

// Work out which game folder to configure.
//
// Normally that is simply the folder this executable sits in. The working
// directory is consulted only as a fallback, and it matters: Wine resolves a
// symlinked executable before reporting it, so an arland-fix-launcher.exe
// symlinked into a game folder reports the link's TARGET as its own location
// and would otherwise configure the build directory it was linked from. The
// working directory stays the folder the tool was started in, which is the
// folder the user means -- Explorer, our own launcher scripts and the
// msimg32 redirect all set it that way.
//
// Returns false when neither folder holds a recognised game, which the caller
// reports to the user.
bool resolveGameFolder() {
  char exeDir[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
  if (!n || n >= MAX_PATH)
    return false;
  char* slash = std::strrchr(exeDir, '\\');
  if (!slash)
    return false;
  slash[1] = '\0';

  int game = gameInFolder(exeDir);
  if (game >= 0) {
    adoptFolder(exeDir, game);
    return true;
  }

  char cwd[MAX_PATH] = {};
  const DWORD c = GetCurrentDirectoryA(MAX_PATH, cwd);
  if (c && c < MAX_PATH - 1) {
    if (cwd[c - 1] != '\\') {
      cwd[c] = '\\';
      cwd[c + 1] = '\0';
    }
    game = gameInFolder(cwd);
    if (game >= 0) {
      adoptFolder(cwd, game);
      return true;
    }
  }

  // No game either side: keep configuring the folder this executable is in,
  // which is where a user who put it somewhere odd would expect the ini.
  adoptFolder(exeDir, -1);
  return false;
}


// Let the user pick a game folder; we edit the arland-fix.ini inside it.
// ---- window construction ---------------------------------------------------

HWND mkLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h,
             DWORD extraStyle = 0) {
  return CreateWindowExW(0, L"STATIC", text,
    WS_CHILD | WS_VISIBLE | extraStyle,
    S(x), S(y), S(w), S(h), parent, nullptr, nullptr, nullptr);
}

HWND mkCheck(HWND parent, const wchar_t* text, int x, int y, int w, int id) {
  return CreateWindowExW(0, L"BUTTON", text,
    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, S(x), S(y), S(w), S(20), parent,
    (HMENU)(INT_PTR)id, nullptr, nullptr);
}

HWND mkCombo(HWND parent, int x, int y, int w, int id) {
  return CreateWindowExW(0, L"COMBOBOX", nullptr,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
    S(x), S(y), S(w), S(200), parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
}

// Start one of Koei Tecmo's own front-ends from the game folder.
//
// ARLAND_NO_REDIRECT is set for the child: msimg32.dll sends
// ArlandDXLauncher.exe here in the first place, so without it that button would
// only ever reopen this window. The variable is removed again immediately, so
// it never reaches the game when Play with mod is pressed afterwards.
bool runStockTool(const char* exeName) {
  if (!g_gameDir[0])
    return false;
  char path[MAX_PATH];
  if (!joinPath(path, g_gameDir, exeName))
    return false;
  if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
    return false;

  SetEnvironmentVariableA("ARLAND_NO_REDIRECT", "1");
  STARTUPINFOA startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  const BOOL started = CreateProcessA(path, nullptr, nullptr, nullptr, FALSE,
    0, nullptr, g_gameDir, &startup, &process);
  SetEnvironmentVariableA("ARLAND_NO_REDIRECT", nullptr);
  if (!started)
    return false;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

// Save, then start the game. `standDownMod` passes ARLAND_DISABLE to the child,
// which makes d3d11.dll forward Direct3D and install nothing: the game as it
// shipped, from the same window, without moving files out of the folder and
// having to remember to move them back.
//
// Returns true when the game started, which is the caller's cue to close.
bool startGame(HWND w, bool standDownMod) {
  // Save first: starting the game with the settings still only on screen is the
  // one outcome nobody wants from either of these buttons. It matters just as
  // much without the mod, since resolution and language live in the game's own
  // settings file and it reads them either way.
  saveToIni();
  // Nothing is pending any more, so a failed launch leaves no close prompt.
  markSaved();

  // Which executable to run follows the language that was just saved, exactly
  // as the game's own launcher decides it. Read from the control rather than
  // the file so it is the selection in front of the user, not a stale one.
  char exePath[MAX_PATH] = {};
  const bool have = gameExeForLanguage(
    comboValue(g_hLang, kLangItems, kLangCount), exePath);

  if (standDownMod)
    SetEnvironmentVariableA("ARLAND_DISABLE", "1");
  // CreateProcess rather than ShellExecute: the game has to be a child of this
  // process for Steam to keep counting the session as running, which is what
  // keeps the overlay and Steam Input attached to it.
  STARTUPINFOA startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  const BOOL started = have && CreateProcessA(exePath, nullptr, nullptr,
    nullptr, FALSE, 0, nullptr, g_gameDir, &startup, &process);
  const DWORD error = GetLastError();
  // Removed immediately, so a later press of Play with mod in this same window
  // cannot inherit it and quietly launch without the mod.
  if (standDownMod)
    SetEnvironmentVariableA("ARLAND_DISABLE", nullptr);

  if (!started) {
    wchar_t failed[320];
    wsprintfW(failed,
      L"The configuration was saved, but %s could not be started (error %lu). "
      L"Launch the game as you normally would; the saved settings still apply.",
      g_gameName ? g_gameName : L"the game",
      have ? error : (DWORD)ERROR_FILE_NOT_FOUND);
    MessageBoxW(w, failed, L"Atelier Arland Fixes", MB_OK | MB_ICONWARNING);
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

// The version of the mod installed beside this tool, read from d3d11.dll's own
// version resource. That file IS the mod, so asking it is more truthful than
// this launcher reporting its own version: the two ship together but nothing
// stops someone updating one and not the other, and when that happens the
// version worth knowing is the DLL's.
const wchar_t* modVersion() {
  static wchar_t version[64] = {};
  if (version[0])
    return version;
  lstrcpynW(version, L"not detected", 64);

  char dll[MAX_PATH] = {};
  if (!fileInDir(g_gameDir, "d3d11.dll", dll))
    return version;

  const DWORD size = GetFileVersionInfoSizeA(dll, nullptr);
  if (!size)
    return version;
  std::vector<BYTE> block(size);
  if (!GetFileVersionInfoA(dll, 0, size, block.data()))
    return version;
  VS_FIXEDFILEINFO* info = nullptr;
  UINT infoSize = 0;
  if (!VerQueryValueW(block.data(), L"\\", (void**)&info, &infoSize) || !info)
    return version;
  // The build field is deliberately not shown: these are versioned major.minor
  // and a trailing 0.0 reads as noise.
  wsprintfW(version, L"%u.%u", HIWORD(info->dwFileVersionMS),
    LOWORD(info->dwFileVersionMS));
  return version;
}

// True when the named executable sits in the game folder, so a button that
// opens it can be greyed out rather than failing when pressed.
bool stockToolPresent(const char* exeName) {
  if (!g_gameDir[0])
    return false;
  char path[MAX_PATH];
  return joinPath(path, g_gameDir, exeName) &&
         GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

HWND mkButton(HWND parent, const wchar_t* text, int x, int y, int w, int id,
              bool isDefault = false) {
  return CreateWindowExW(0, L"BUTTON", text,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP |
      (isDefault ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
    S(x), S(y), S(w), S(26), parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
}

// The font the running version of Windows actually uses for UI text, which is
// Segoe UI on 10 and 11 and whatever succeeds it later. Asking the OS is the
// only way that stays right across versions; DEFAULT_GUI_FONT is still the
// 1990s bitmap face and is the single biggest reason a plain Win32 window looks
// dated. Falls back to it only if the query fails.
// Register the bundled font with this process only. AddFontMemResourceEx makes
// it available to CreateFont by family name without installing anything on the
// system or touching a file, and it is released when the process exits.
bool registerBundledFont() {
  static const bool registered = [] {
    DWORD faces = 0;
    return AddFontMemResourceEx(
      const_cast<unsigned char*>(atfix::kEmbeddedFontInter),
      atfix::kEmbeddedFontInterSize, nullptr, &faces) != nullptr;
  }();
  return registered;
}

// The font the window draws in.
//
// Asking the OS (SPI_GETNONCLIENTMETRICS) gets Segoe UI on Windows, which is
// right, and whatever Wine defaults to elsewhere -- usually Tahoma or a DejaVu
// fallback. That is the whole reason this window looked considerably worse on
// SteamOS than on Windows: same code, different face. Bundling one font and
// using it on both makes them agree, and it is the same Nunito the DLL already
// embeds, so it costs a licence we already ship and no new dependency.
//
// The system metrics are still consulted, for the SIZE: that is what carries
// the display's DPI and the user's "make text bigger" preference, both of which
// should still be obeyed. Only the face is replaced. If registration fails the
// system font is used unchanged, so a missing font is a cosmetic fallback
// rather than an unreadable window.
//
// Inter is the face because it was drawn for user interfaces at small sizes,
// which is exactly this window: a tall x-height, open apertures, and shapes
// that stay distinct at 12px where a text face goes muddy.
HFONT createUiFont() {
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
      &metrics, 0)) {
    LOGFONTW font = metrics.lfMessageFont;
    // g_userScale only: this height already carries the display's DPI, since
    // the process is DPI aware. See the note on the two scale factors.
    font.lfHeight = font.lfHeight * g_userScale / 100;
    // ClearType explicitly rather than DEFAULT_QUALITY, which under Wine can
    // resolve to unsmoothed rendering and is what makes small text look ragged
    // there even with a good face.
    font.lfQuality = CLEARTYPE_QUALITY;
    if (registerBundledFont()) {
      lstrcpynW(font.lfFaceName, L"Inter", LF_FACESIZE);
      // Rendered a size up from what the system asked for. Not compensation --
      // Inter's x-height is close to Segoe UI's, so it needs none -- but a
      // deliberate choice: this window is read at arm's length on a handheld or
      // across a room on a TV as often as it is at a desk, and the default
      // desktop size is small for both. It scales WITH the user's text-size
      // setting rather than replacing it, so someone who has already asked for
      // larger text still gets larger text.
      font.lfHeight = font.lfHeight * 115 / 100;
    }
    if (HFONT created = CreateFontIndirectW(&font))
      return created;
  }
  return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

// The DPI the window will be laid out at. GetDpiForSystem is Windows 10 and
// later; the desktop DC gives the same answer everywhere else, including Wine.
int systemDpi() {
  if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
    using PFN_GetDpiForSystem = UINT (WINAPI*)();
    if (auto getDpi = (PFN_GetDpiForSystem)GetProcAddress(user32,
          "GetDpiForSystem"))
      return (int)getDpi();
  }
  HDC screen = GetDC(nullptr);
  const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
  if (screen) ReleaseDC(nullptr, screen);
  return dpi > 0 ? dpi : 96;
}

// The work area of the monitor the window will open on (the one holding the
// cursor), which is what the layout has to fit inside.
bool cursorWorkArea(RECT* area) {
  POINT cursor = {};
  MONITORINFO monitor = {};
  monitor.cbSize = sizeof(monitor);
  if (!GetCursorPos(&cursor) ||
      !GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY),
                       &monitor))
    return false;
  *area = monitor.rcWork;
  return true;
}

// Decide both scale factors, then reduce the enlargement until the window fits
// the screen it is opening on. The base layout is sized for 720p, so this can
// always fall back to no enlargement at all and still fit.
//
// ARLAND_UI_SCALE sets the enlargement directly, as a percentage. It is how the
// large layout gets tested without a TV, and how someone on a TV that Steam has
// not told us about can ask for it.
void chooseScale(DWORD windowStyle) {
  g_dpiScale = systemDpi() * 100 / 96;
  if (g_dpiScale < 100)
    g_dpiScale = 100;

  g_userScale = 100;
  if (const char* requested = std::getenv("ARLAND_UI_SCALE")) {
    const int value = std::atoi(requested);
    if (value >= 100 && value <= 200)
      g_userScale = value;
  }

  RECT area = {};
  if (!cursorWorkArea(&area))
    return;
  const int availableWidth = area.right - area.left;
  const int availableHeight = area.bottom - area.top;
  while (g_userScale > 100) {
    RECT window = { 0, 0, S(kBaseWidth), S(kBaseHeight) };
    AdjustWindowRect(&window, windowStyle, FALSE);
    if (window.right - window.left <= availableWidth &&
        window.bottom - window.top <= availableHeight)
      break;
    g_userScale -= 5;
  }
}

// Whether this is running under Wine, which on the Steam Deck and on Linux
// generally means Proton. The canonical test: Wine's ntdll exports a version
// function no Windows one has. Used only to word a note, so a wrong answer
// costs nothing.
bool runningUnderWine() {
  static const bool wine = [] {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
  }();
  return wine;
}

// Wear the icon of whichever game this folder holds. The window is about that
// game, and on a taskbar with several of these open it is the only thing that
// tells them apart. Nothing to clean up: the icons live as long as the process.
void applyGameIcon(HWND w) {
  if (!g_gameExePath[0])
    return;
  // Not named "small": the Windows headers define that as a macro for char,
  // which MinGW tolerates here and MSVC does not.
  HICON largeIcon = nullptr;
  HICON smallIcon = nullptr;
  ExtractIconExA(g_gameExePath, 0, &largeIcon, &smallIcon, 1);
  if (largeIcon)
    SendMessageW(w, WM_SETICON, ICON_BIG, (LPARAM)largeIcon);
  if (smallIcon)
    SendMessageW(w, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
}

void applyFont(HWND parent) {
  // Use the system UI font on every child.
  for (HWND c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
    SendMessageW(c, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
}

// Remember a control so the tab can show or hide it with its page.
void onPage(int page, HWND ctrl) {
  if (ctrl && g_pageCount[page] < 40)
    g_pageCtrls[page][g_pageCount[page]++] = ctrl;
}


// A one-line note under the control it explains: indented past the control's
// label so it reads as subordinate, and drawn grey (see WM_CTLCOLORSTATIC).
HWND mkDesc(HWND parent, const wchar_t* text, int x, int y, int width) {
  // Two lines' worth of height: a static word-wraps on its own, so a longer
  // note reflows instead of being clipped. Rows are pitched to suit.
  HWND h = mkLabel(parent, text, x, y, width, 32);
  if (g_descCount < 20)
    g_hDesc[g_descCount++] = h;
  return h;
}

// Repaint the whole area a control covers, background included. The labels are
// drawn without a background of their own (see WM_CTLCOLORSTATIC), so whatever
// they had before stays on screen until the tab page underneath is redrawn --
// which needs the parent, the tab control and the label itself, in that order.
void repaintUnder(HWND ctrl) {
  HWND parent = GetParent(ctrl);
  if (!parent)
    return;
  RECT area;
  GetWindowRect(ctrl, &area);
  MapWindowPoints(nullptr, parent, (POINT*)&area, 2);
  RedrawWindow(parent, &area, nullptr,
    RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void showPage(int page) {
  for (int p = 0; p < 4; ++p)
    for (int i = 0; i < g_pageCount[p]; ++i)
      ShowWindow(g_pageCtrls[p][i], p == page ? SW_SHOW : SW_HIDE);
  // The outgoing page's labels leave their text behind them, so the page is
  // redrawn as a whole rather than relying on each control to clean up after
  // itself.
  if (g_hTabs)
    repaintUnder(g_hTabs);
}


// ---- layout ----------------------------------------------------------------
//
// Controls are stacked by a cursor instead of being placed at hand-picked
// coordinates, and every note's height is MEASURED at the width it will be
// drawn at.
//
// That measurement is the point of this. A static silently drops any line past
// its height, so a note given a fixed two lines turns a third line into a
// sentence ending mid-word -- with nothing in the build, the log or the code to
// say so. It shipped that way twice. Measuring makes the row as tall as its
// text, which also means notes can be reworded and settings reordered without
// recomputing anything below them.
//
// Everything in here is DEVICE pixels. The logical constants go through S() on
// the way in, so nothing downstream has to track which of the two it is
// holding -- the other half of the same class of bug.
struct Layout {
  HWND parent;
  int page;
  int y;

  Layout(HWND parentWindow, int tabPage)
    : parent(parentWindow), page(tabPage), y(S(46)) {}

  // Columns, in logical units. Checkbox rows use a nearer note column: a
  // checkbox carries its own label, so leaving its note out at the combo note
  // column strands it across a gap of empty space.
  static int left()          { return S(24); }
  static int labelWidth()    { return S(150); }
  static int controlLeft()   { return S(180); }
  static int controlWidth()  { return S(230); }
  static int noteLeft()      { return S(430); }
  static int noteWidth()     { return S(250); }
  static int checkNoteLeft() { return S(300); }
  static int checkNoteWidth(){ return S(376); }
  static int fullWidth()     { return S(656); }

  int measure(const wchar_t* text, int width) const {
    HDC dc = GetDC(parent);
    if (!dc)
      return S(32);
    HFONT previous = (HFONT)SelectObject(dc, g_uiFont);
    RECT box = { 0, 0, width, 0 };
    DrawTextW(dc, text, -1, &box, DT_CALCRECT | DT_WORDBREAK);
    SelectObject(dc, previous);
    ReleaseDC(parent, dc);
    return box.bottom;
  }

  HWND place(const wchar_t* cls, const wchar_t* text, DWORD style,
             int x, int top, int width, int height) {
    HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
      x, top, width, height, parent, nullptr, nullptr, nullptr);
    onPage(page, control);
    return control;
  }

  // A note, at its measured height, remembered so WM_CTLCOLORSTATIC draws it in
  // the secondary colour. Returns the height it took.
  int note(const wchar_t* text, int x, int width, int top) {
    const int height = measure(text, width);
    HWND label = place(L"STATIC", text, 0, x, top, width, height);
    if (g_descCount < 32)
      g_hDesc[g_descCount++] = label;
    return height;
  }

  // label + control + note. The row is as tall as whichever of the two sides
  // needs more room.
  void row(const wchar_t* labelText, HWND control, const wchar_t* noteText) {
    place(L"STATIC", labelText, 0, left(), y + S(4), labelWidth(), S(18));
    // Combos are moved with their dropdown extent, not their visible height:
    // for a combo box the height passed here is how far the list drops.
    MoveWindow(control, controlLeft(), y, controlWidth(), S(200), TRUE);
    onPage(page, control);
    int used = S(24);
    if (noteText)
      used = std::max(used, note(noteText, noteLeft(), noteWidth(), y));
    y += used + S(12);
  }

  void checkRow(HWND check, const wchar_t* noteText) {
    MoveWindow(check, left(), y + S(2), checkNoteLeft() - left() - S(16), S(20),
      TRUE);
    onPage(page, check);
    int used = S(22);
    if (noteText)
      used = std::max(used, note(noteText, checkNoteLeft(), checkNoteWidth(), y));
    y += used + S(12);
  }

  void heading(const wchar_t* text) {
    y += S(8);
    place(L"STATIC", text, 0, left(), y, fullWidth(), S(18));
    y += S(24);
  }

  // Full-width text in the primary colour: a statement of fact rather than an
  // explanation of a control, so it does not get the notes' grey.
  void label(const wchar_t* text) {
    const int height = measure(text, fullWidth());
    place(L"STATIC", text, 0, left(), y, fullWidth(), height);
    y += height + S(8);
  }

  // A note that belongs to the page rather than to one control.
  void fullNote(const wchar_t* text) {
    y += note(text, left(), fullWidth(), y) + S(12);
  }

  void buttons(HWND a, HWND b, HWND c) {
    const int width = S(150);
    const int gap = S(12);
    MoveWindow(a, left(), y, width, S(26), TRUE);
    MoveWindow(b, left() + width + gap, y, width, S(26), TRUE);
    MoveWindow(c, left() + 2 * (width + gap), y, width, S(26), TRUE);
    onPage(page, a); onPage(page, b); onPage(page, c);
    y += S(26) + S(12);
  }

  // A line belonging to the control above it, so it starts at the control
  // column rather than the label margin.
  void under(HWND control) {
    MoveWindow(control, controlLeft(), y,
      fullWidth() - (controlLeft() - left()), S(18), TRUE);
    onPage(page, control);
    y += S(18) + S(10);
  }

  void link(HWND control) {
    MoveWindow(control, left(), y, fullWidth(), S(20), TRUE);
    onPage(page, control);
    y += S(20) + S(12);
  }
};

void createControls(HWND w) {
  g_hTabs = CreateWindowExW(0, WC_TABCONTROLW, nullptr,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(12), S(12), S(676), S(376),
    w, (HMENU)(INT_PTR)IDC_TABS, nullptr, nullptr);
  SetWindowSubclass(g_hTabs, TabProc, 0, 0);
  TCITEMW tab = {};
  tab.mask = TCIF_TEXT;
  const wchar_t* pageNames[4] = {
    L"Display", L"Image Quality", L"Game", L"About" };
  for (int i = 0; i < 4; ++i) {
    tab.pszText = (LPWSTR)pageNames[i];
    SendMessageW(g_hTabs, TCM_INSERTITEMW, i, (LPARAM)&tab);
  }

  // Which game this folder is: the tool configures whatever it sits next to, so
  // this is the one fact worth stating outright. It sits on the tab strip's own
  // row, right-aligned, where it reads as a heading for the window rather than
  // competing with the tabs. Created AFTER the tab control so it is above it in
  // z-order, and painted transparently (see WM_CTLCOLORSTATIC) so the strip
  // shows through instead of a white block sitting on it.
  wchar_t heading[160];
  wsprintfW(heading, L"%s", g_gameName ? g_gameName : L"No game detected");
  g_hGameLabel = mkLabel(w, heading, 360, 18, 320, 18, SS_RIGHT);

  // ---------------- page 0: Display ----------------
  {
    Layout page(w, 0);
    g_hBase = mkCombo(w, 0, 0, 10, IDC_BASE);
    unsigned maxW = 0, maxH = 0;
    displayMaximum(&maxW, &maxH);
    for (int i = 0; i < kBaseCount; ++i) {
      // Skip anything the display cannot show. Auto (0x0) always stays.
      if (maxW && maxH && kBaseItems[i].w &&
          (kBaseItems[i].w > maxW || kBaseItems[i].h > maxH))
        continue;
      int idx = (int)SendMessageW(g_hBase, CB_ADDSTRING, 0,
        (LPARAM)kBaseItems[i].label);
      SendMessageW(g_hBase, CB_SETITEMDATA, idx,
        packRes(kBaseItems[i].w, kBaseItems[i].h));
    }
    page.row(L"Resolution:", g_hBase,
      L"What reaches the screen. Also written to the game's own settings.");

    g_hWinMode = mkCombo(w, 0, 0, 10, IDC_WINMODE);
    for (int i = 0; i < kWindowModeCount; ++i)
      SendMessageW(g_hWinMode, CB_ADDSTRING, 0, (LPARAM)kWindowModes[i].label);
    page.row(L"Window mode:", g_hWinMode,
      L"Borderless fills the monitor without taking over the display, so "
      L"alt-tab is instant.");

    // A statement of fact rather than a setting, so it spans the width.
    page.fullNote(runningUnderWine()
      ? L"The game runs at your display's refresh rate, 120 Hz and 144 Hz "
        L"included. The mod does not cap the frame rate, so a limit set by "
        L"Steam or the compositor is respected."
      : L"The game runs at your display's refresh rate, 120 Hz and 144 Hz "
        L"included. The mod does not cap the frame rate.");

    // The stock front-ends are still reachable: this tool replaces them, it
    // does not remove them. Greyed out when the executable is not present.
    page.heading(L"The game as it shipped");
    HWND openEnv = mkButton(w, L"Settings &editor", 0, 0, 10, IDC_OPENENV);
    HWND openLauncher = mkButton(w, L"&Original launcher", 0, 0, 10,
      IDC_OPENLAUNCHER);
    HWND playVanilla = mkButton(w, L"Play &without the mod", 0, 0, 10,
      IDC_PLAYVANILLA);
    page.buttons(openEnv, openLauncher, playVanilla);
    if (!stockToolPresent("ArlandDXEnv.exe"))
      EnableWindow(openEnv, FALSE);
    if (!stockToolPresent("ArlandDXLauncher.exe"))
      EnableWindow(openLauncher, FALSE);
    if (!g_gameExePath[0])
      EnableWindow(playVanilla, FALSE);
    page.fullNote(
      L"Koei Tecmo's own settings editor and launcher, unmodified. The third "
      L"saves and starts the game with the mod stood down, changing nothing.");
  }

  // ---------------- page 1: Image Quality ----------------
  {
    Layout page(w, 1);
    g_hPreset = mkCombo(w, 0, 0, 10, IDC_PRESET);
    for (int i = 0; i < kPresetCount; ++i)
      SendMessageW(g_hPreset, CB_ADDSTRING, 0, (LPARAM)kPresets[i].label);
    page.row(L"Preset:", g_hPreset, L"Sets the four options below.");

    g_hSS = mkCombo(w, 0, 0, 10, IDC_SS);
    refillSupersampling();
    page.row(L"Supersampling:", g_hSS,
      L"Renders higher, then scales down. The sharpest, and the costliest. "
      L"Limited to 8K.");

    // The live readout under the supersampling row. Registered as a note so it
    // draws in the secondary colour, but it is not created by note(): its text
    // changes at runtime, so it keeps a fixed height rather than being measured
    // once against a string it will not be showing later.
    g_hRendLbl = mkLabel(w, L"", 0, 0, 10, 10);
    if (g_descCount < 32)
      g_hDesc[g_descCount++] = g_hRendLbl;
    page.under(g_hRendLbl);

    g_hMsaa = mkCombo(w, 0, 0, 10, IDC_MSAA);
    comboFill(g_hMsaa, kMsaaItems, 4);
    page.row(L"Anti-aliasing (MSAA):", g_hMsaa,
      L"MSAA. Smooths geometry edges; supersampling usually beats it.");

    g_hAniso = mkCombo(w, 0, 0, 10, IDC_ANISO);
    comboFill(g_hAniso, kAnisoItems, 5);
    page.row(L"Texture sharpness:", g_hAniso,
      L"Anisotropic filtering. Keeps floors and walls sharp at a shallow "
      L"angle, and costs nothing per frame.");

    g_hShadow = mkCombo(w, 0, 0, 10, IDC_SHADOW);
    comboFill(g_hShadow, kShadowItems, 4);
    page.row(L"Shadow detail:", g_hShadow,
      L"Larger shadow maps, so sharper shadow edges. Costs video memory.");

    g_hSmaa = mkCheck(w, L"Edge smoothing", 0, 0, 10, IDC_SMAA);
    page.checkRow(g_hSmaa,
      L"SMAA post-process. Cheap, and catches edges MSAA cannot.");
  }

  // ---------------- page 2: Game ----------------
  {
    Layout page(w, 2);
    g_hFont = mkCombo(w, 0, 0, 10, IDC_FONT);
    comboFill(g_hFont, kFontItems, 3);
    page.row(L"UI font:", g_hFont,
      L"Re-renders the game's UI text from a bundled font. English builds "
      L"only.");

    g_hLang = mkCombo(w, 0, 0, 10, IDC_LANG);
    comboFill(g_hLang, kLangItems, kLangCount);
    page.row(L"Language:", g_hLang,
      L"Written to the game's own settings, and decides which of the game's "
      L"two executables starts.");

    page.heading(L"Battle");
    g_hBShadow = mkCheck(w, L"Character shadows in battle", 0, 0, 10,
      IDC_BSHADOW);
    page.checkRow(g_hBShadow,
      L"Restores the shadows Rorona is missing while fighting.");
    g_hBCutInShadow = mkCheck(w, L"Ground shadows during cut-ins", 0, 0, 10,
      IDC_BCUTINSHADOW);
    page.checkRow(g_hBCutInShadow,
      L"Off by default; the vanilla cut-ins have no shadows.");
    g_hBCutInDim = mkCheck(w, L"Dim the scene during cut-ins", 0, 0, 10,
      IDC_BCUTINDIM);
    page.checkRow(g_hBCutInDim,
      L"Uncheck to hold close-ups at full brightness.");

    page.heading(L"Diagnostics");
    g_hVerbose = mkCheck(w, L"Verbose logging", 0, 0, 10, IDC_VERBOSE);
    page.checkRow(g_hVerbose,
      L"Extra detail in arland-fix.log. Crash reports are always written.");
  }

  // ---------------- page 3: About ----------------
  // What is installed, where it came from, and what it is not.
  {
    Layout page(w, 3);
    wchar_t installed[160];
    wsprintfW(installed, L"Mod version: %s", modVersion());
    page.label(installed);
    page.fullNote(
      L"Read from d3d11.dll in this folder, which is the mod itself. "
      L"\u201cNot detected\u201d there means the game is running unmodified.");
    page.fullNote(
      L"Free and open source, and not affiliated with or endorsed by Koei "
      L"Tecmo or Gust. It is an unofficial attempt to fix bugs in the "
      L"original games, and it is never sold.");

    // A SysLink rather than a static: it is focusable, so it can be reached
    // with the keyboard or a controller, and it draws in the system's link
    // colour instead of an imitation of one.
    wchar_t markup[320];
    wsprintfW(markup, L"<a href=\"%s\">%s</a>", kRepositoryUrl, kRepositoryUrl);
    g_hRepoLink = CreateWindowExW(0, WC_LINK, markup,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, w,
      (HMENU)(INT_PTR)IDC_REPOLINK, nullptr, nullptr);
    page.link(g_hRepoLink);
  }

  // Bottom left, away from Save/Close so it cannot be hit by accident: saves
  // first, then launches. Disabled when no game was recognised in this folder.
  //
  // It is also the default button and takes focus at startup: most of the time
  // this window is opened on the way into the game, not to change something, so
  // Enter should start playing. That matters most on a controller or a
  // handheld, where the alternative is driving a cursor across the window.
  g_hStart = mkButton(w, L"&Play with mod", 24, 400, 130, IDC_START, true);
  if (!g_gameExePath[0])
    EnableWindow(g_hStart, FALSE);

  // Distinct mnemonics across the whole window: P play, R reset, C close,
  // E editor, O original launcher, W play without the mod.
  mkButton(w, L"&Reset to defaults", 456, 400, 130, IDC_RESET);
  mkButton(w, L"&Close", 594, 400, 90, IDC_CLOSE);

  showPage(0);

  applyFont(w);
}

// The tab control paints its own page background from the visual style: white
// under Wine, but a light grey on Windows. Everything sitting on that page is
// painted white (see kBackground), so on Windows the controls appeared as white
// patches on a grey panel.
//
// This paints the page white and leaves the tab strip alone. The default
// handler runs first, so the tabs, their selected state and the border are all
// still drawn by the theme; only the display area is then filled.
// TCM_ADJUSTRECT is what says where that area is, rather than guessing at the
// header height.
LRESULT CALLBACK TabProc(HWND tabs, UINT msg, WPARAM wp, LPARAM lp,
                         UINT_PTR, DWORD_PTR) {
  if (msg == WM_ERASEBKGND) {
    const LRESULT handled = DefSubclassProc(tabs, msg, wp, lp);
    RECT page = {};
    GetClientRect(tabs, &page);
    SendMessageW(tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&page);
    FillRect((HDC)wp, &page, g_backgroundBrush);
    return handled;
  }
  if (msg == WM_NCDESTROY)
    RemoveWindowSubclass(tabs, TabProc, 0);
  return DefSubclassProc(tabs, msg, wp, lp);
}

LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      createControls(w);
      loadFromIni();
      markSaved();
      return 0;

    case WM_NOTIFY: {
      const NMHDR* note = (const NMHDR*)lp;
      if (note && note->hwndFrom == g_hRepoLink &&
          (note->code == NM_CLICK || note->code == NM_RETURN)) {
        ShellExecuteW(w, L"open", kRepositoryUrl, nullptr, nullptr,
          SW_SHOWNORMAL);
        return 0;
      }
      if (note && note->hwndFrom == g_hTabs && note->code == TCN_SELCHANGE) {
        showPage((int)SendMessageW(g_hTabs, TCM_GETCURSEL, 0, 0));
        return 0;
      }
      break;
    }
    // Checkboxes send WM_CTLCOLORBTN rather than WM_CTLCOLORSTATIC, and a
    // themed one ignores a hollow brush and fills with the dialog face anyway,
    // so both messages are answered the same way: with the colour the tab page
    // is actually painted in.
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC: {
      // One background for the whole window, so there is no on-the-page versus
      // off-the-page distinction to get wrong. Checkboxes answer here through
      // WM_CTLCOLORBTN rather than WM_CTLCOLORSTATIC, which is why both are
      // handled together.
      // The game name lies on top of the tab control's own header strip, which
      // is not the window background, so it takes no background of its own.
      if ((HWND)lp == g_hGameLabel) {
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, kSecondaryText);
        return (LRESULT)GetStockObject(NULL_BRUSH);
      }
      SetBkColor((HDC)wp, kBackground);
      SetTextColor((HDC)wp, kText);
      // The one-line notes under each control are secondary text, so they are
      // drawn grey rather than competing with the labels they explain.
      for (int i = 0; i < g_descCount; ++i) {
        if ((HWND)lp == g_hDesc[i]) {
          SetTextColor((HDC)wp, kSecondaryText);
          break;
        }
      }
      return (LRESULT)g_backgroundBrush;
    }
    case WM_COMMAND:
      // Base or supersampling changed: recompute the render label and the
      // Auto-disables-supersampling rule live.
      if (HIWORD(wp) == CBN_SELCHANGE && LOWORD(wp) == IDC_PRESET) {
        applyPreset((int)SendMessageW(g_hPreset, CB_GETCURSEL, 0, 0));
        updateRenderResolution();
        return 0;
      }
      // Any hand-edited quality setting means the preset no longer describes
      // what is selected, so the box drops to Custom rather than lying.
      if ((HIWORD(wp) == CBN_SELCHANGE &&
           (LOWORD(wp) == IDC_SS || LOWORD(wp) == IDC_MSAA ||
            LOWORD(wp) == IDC_ANISO || LOWORD(wp) == IDC_SHADOW)) ||
          (HIWORD(wp) == BN_CLICKED && LOWORD(wp) == IDC_SMAA)) {
        refreshPreset();
        updateRenderResolution();
        return 0;
      }
      if (HIWORD(wp) == CBN_SELCHANGE && LOWORD(wp) == IDC_BASE) {
        updateRenderResolution();
        return 0;
      }
      switch (LOWORD(wp)) {
        case IDC_RESET: {
          // Destructive and not undoable, so it asks first, and the question
          // names what it will and will not touch. Defaulting to No: this sits
          // next to Close, and the cost of a mis-click is someone's whole
          // configuration.
          const int answer = MessageBoxW(w,
            L"Reset the configuration of the mod back to the defaults? This "
            L"will also set your game resolution back to 1280x720 in windowed "
            L"mode.",
            L"Atelier Arland Fixes",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
          if (answer != IDYES)
            return 0;
          resetToDefaults();
          saveToIni();
          markSaved();
          // Name the game back to the user: this tool configures whichever
          // folder it sits in, and saying which one closes that loop.
          wchar_t saved[320];
          wsprintfW(saved,
            L"The settings have been reset to the mod's defaults and saved. "
            L"The next time you launch %s they will be used.",
            g_gameName ? g_gameName : L"the game");
          MessageBoxW(w, saved, L"Atelier Arland Fixes", MB_OK | MB_ICONINFORMATION);
          return 0;
        }
        case IDC_START:
        case IDC_PLAYVANILLA:
          if (startGame(w, LOWORD(wp) == IDC_PLAYVANILLA))
            DestroyWindow(w);
          return 0;
        case IDC_OPENENV:
        case IDC_OPENLAUNCHER: {
          const bool env = LOWORD(wp) == IDC_OPENENV;
          const char* exeName =
            env ? "ArlandDXEnv.exe" : "ArlandDXLauncher.exe";
          if (!runStockTool(exeName)) {
            wchar_t failed[256];
            wsprintfW(failed,
              L"%s could not be started. It may have been moved or removed "
              L"from the game folder.",
              env ? L"The settings editor" : L"The original launcher");
            MessageBoxW(w, failed, L"Atelier Arland Fixes", MB_OK | MB_ICONWARNING);
          }
          return 0;
        }
        case IDC_CLOSE:
        // IsDialogMessage turns Escape into IDCANCEL, so this is the Escape
        // key. Both go through WM_CLOSE so the unsaved-changes check sits in
        // one place, shared with the window's own close button.
        case IDCANCEL:
          SendMessageW(w, WM_CLOSE, 0, 0);
          return 0;
      }
      return 0;

    case WM_CLOSE:
      // Settings are written when the game starts, so closing after an edit
      // would throw it away with no sign that it happened. Cancel is the
      // default: of the three answers it is the only one that loses nothing.
      if (hasUnsavedChanges()) {
        const int answer = MessageBoxW(w,
          L"Save the changes you made to the settings before closing? They "
          L"are normally written when you start the game.",
          L"Atelier Arland Fixes",
          MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON3);
        if (answer == IDCANCEL)
          return 0;
        if (answer == IDYES)
          saveToIni();
      }
      DestroyWindow(w);
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(w, msg, wp, lp);
}

}  // namespace

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
  INITCOMMONCONTROLSEX icc = { sizeof(icc),
    ICC_STANDARD_CLASSES | ICC_LINK_CLASS };
  InitCommonControlsEx(&icc);

  // Fixed-size dialog-style window (no maximize / resize). Declared here
  // because the scale has to know the frame it will be measured with.
  const DWORD windowStyle =
    (WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME)) | WS_VISIBLE;
  chooseScale(windowStyle);
  g_uiFont = createUiFont();
  g_backgroundBrush = CreateSolidBrush(kBackground);

  // This tool edits the files beside it, so both must be there. Saying which
  // one is missing is the whole of the diagnosis for a misplaced copy.
  if (!resolveGameFolder()) {
    MessageBoxW(nullptr,
      L"No Atelier Arland game was found in this folder.\n\n"
      L"Put arland-fix-launcher.exe in the game's installation folder, "
      L"beside the game executable and d3d11.dll, and run it from there.",
      L"Atelier Arland Fixes", MB_OK | MB_ICONERROR);
    return 1;
  }
  if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
    MessageBoxW(nullptr,
      L"arland-fix.ini was not found in this folder.\n\n"
      L"It ships in the release archive alongside the DLLs. Copy it in "
      L"next to the game executable, or launch the game once to have the "
      L"mod create one.",
      L"Atelier Arland Fixes", MB_OK | MB_ICONERROR);
    return 1;
  }

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = g_backgroundBrush;
  wc.lpszClassName = L"ArlandConfigWindow";
  RegisterClassExW(&wc);

  const DWORD style = windowStyle;
  RECT r = { 0, 0, S(kBaseWidth), S(kBaseHeight) };
  AdjustWindowRect(&r, style, FALSE);
  const int width = r.right - r.left;
  const int height = r.bottom - r.top;

  // Centred rather than left to the default cascade position. This window is
  // the first thing seen when the game is started, so it should arrive where
  // the eye already is. Centred on the monitor holding the cursor, and on that
  // monitor's work area rather than its full bounds, so a taskbar cannot push
  // the lower buttons off-screen. Falls back to the default position if the
  // monitor cannot be identified.
  int x = CW_USEDEFAULT;
  int y = CW_USEDEFAULT;
  POINT cursor = {};
  MONITORINFO monitor = {};
  monitor.cbSize = sizeof(monitor);
  if (GetCursorPos(&cursor) &&
      GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY),
                      &monitor)) {
    x = monitor.rcWork.left +
        ((monitor.rcWork.right - monitor.rcWork.left) - width) / 2;
    y = monitor.rcWork.top +
        ((monitor.rcWork.bottom - monitor.rcWork.top) - height) / 2;
  }

  // Name the game in the title, not just in the window: with the game's icon
  // beside it this is what identifies the right one on a taskbar holding more
  // than one of these.
  wchar_t title[192];
  if (g_gameName)
    wsprintfW(title, L"%s - Atelier Arland Fixes", g_gameName);
  else
    lstrcpynW(title, L"Atelier Arland Fixes", 192);

  HWND w = CreateWindowExW(0, wc.lpszClassName,
    title, style,
    x, y, width, height,
    nullptr, nullptr, hInst, nullptr);
  if (!w)
    return 1;
  applyGameIcon(w);
  // After the controls exist and the window is up, so nothing takes it back.
  // Skipped when there is no game to start, since focus on a disabled control
  // would leave the keyboard nowhere.
  if (g_hStart && IsWindowEnabled(g_hStart))
    SetFocus(g_hStart);

  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(w, &m)) {   // Tab / arrow navigation between controls
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  }
  return (int)m.wParam;
}
