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
#include <cstring>

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
  IDC_SAVE,
  IDC_CLOSE,
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
HWND g_hDesc[20] = {};   // greyed one-line notes; drawn in COLOR_GRAYTEXT
int  g_descCount = 0;
HWND g_pageCtrls[3][40] = {};   // which controls belong to which tab page
int  g_pageCount[3] = {};
HWND g_hStart = nullptr;   // focused at startup; see createControls
HWND g_hPreset, g_hWinMode, g_hLang,
     g_hFont, g_hBase, g_hSS, g_hRendLbl, g_hMsaa, g_hShadow,
     g_hAniso, g_hSmaa, g_hBShadow, g_hBCutInShadow, g_hBCutInDim, g_hVerbose;

HFONT g_uiFont = nullptr;

// The dropdown entries. The first column is the label shown to the user, the
// second is the exact string written to the ini.
struct ComboItem { const wchar_t* label; const char* value; };

const ComboItem kFontItems[] = {
  { L"replaced (embedded scalable font, default)", "replaced" },
  { L"upscaled (smooth the original glyphs)",      "upscaled" },
  { L"original (untouched bitmap font)",           "original" },
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
// 4K. Above it the engine's own render targets stop fitting in video memory on
// real hardware -- a 4K panel at 4x is 15360x8640, over half a gigabyte for a
// single target, and the games allocate many. What that looks like in play is
// not a clean failure: some targets still allocate and some do not, so the
// game runs but a conversation draws black and the frame rate hitches every
// few seconds as video memory is paged. Multipliers that would exceed this are
// not offered (see refillSupersampling), and the mod clamps the ini as well.
const unsigned kMaxRenderWidth = 7680;
const unsigned kMaxRenderHeight = 4320;

// Quality presets. These set only the Image quality group -- resolution,
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
// Balanced matches default.ini exactly, so a fresh install opens on a named
// preset rather than on Custom. The higher tiers add the cheap wins first
// (anisotropic filtering costs nothing per frame, shadows little) before
// spending on supersampling, which costs the most.
const Preset kPresets[] = {
  //                        SS  MSAA  SMAA  aniso  shadow
  { L"Balanced (default)",   0,   0,  true,     0,      0 },
  { L"High",                 2,   0,  true,     4,      1 },
  { L"Maximum",              3,   0,  true,     4,      2 },
  { L"Custom",              -1,  -1,  true,    -1,     -1 },
};
const int kPresetCount = 4;
const int kPresetCustom = 3;

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
void refillSupersampling() {
  const int wanted = ssIndex();
  unsigned bw = 0, bh = 0;
  const bool haveBase = selectedBase(&bw, &bh);
  SendMessageW(g_hSS, CB_RESETCONTENT, 0, 0);
  for (int i = 0; i < kSSCount; ++i) {
    if (i && haveBase && !withinRenderLimit(bw, bh, kSSItems[i].mult))
      continue;
    const int at =
      (int)SendMessageW(g_hSS, CB_ADDSTRING, 0, (LPARAM)kSSItems[i].label);
    SendMessageW(g_hSS, CB_SETITEMDATA, at, i);
  }
  setSsIndex(wanted);
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

void updateRenderResolution() {
  unsigned bw, bh;
  bool haveBase = selectedBase(&bw, &bh);
  refillSupersampling();
  EnableWindow(g_hSS, haveBase);
  if (!haveBase)
    SendMessageW(g_hSS, CB_SETCURSEL, 0, 0);   // force Off

  char text[96];
  unsigned rw, rh;
  if (!haveBase)
    lstrcpyA(text, "Render resolution: base is Auto (supersampling off)");
  else if (!computeRender(&rw, &rh))
    lstrcpyA(text, "Render resolution: (off, renders at base)");
  else
    wsprintfA(text, "Render resolution: %u x %u", rw, rh);
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

  // MSAA / ShadowMultiplier default to 1 (off); Aniso to 1 (off, the mod
  // treats 0/1/absent identically as off).
  iniString("Rendering", "MSAA", buf, sizeof(buf));
  comboSelectByValue(g_hMsaa, kMsaaItems, 4, buf[0] ? buf : "1", 0);
  iniString("Rendering", "ShadowMultiplier", buf, sizeof(buf));
  comboSelectByValue(g_hShadow, kShadowItems, 4, buf[0] ? buf : "1", 0);
  iniString("Rendering", "AnisotropicFiltering", buf, sizeof(buf));
  comboSelectByValue(g_hAniso, kAnisoItems, 5, buf[0] ? buf : "1", 0);

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

// Join `dir` (which must end in a separator) and `name` into `out`, and say
// whether the result exists.
bool fileInDir(const char* dir, const char* name, char* out) {
  lstrcpynA(out, dir, MAX_PATH);
  lstrcatA(out, name);
  return GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
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
  lstrcpynA(g_settingsPath, dir, MAX_PATH);
  lstrcatA(g_settingsPath, "ArlandDX_Settings.ini");
  lstrcpynA(g_gameDir, dir, MAX_PATH);   // keeps its trailing separator
  g_game = game;
  if (game < 0)
    return;
  g_gameName = kGames[game].name;
  // For the icon and for whether Start game can do anything at all; which
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

HWND mkLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
  return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
    x, y, w, h, parent, nullptr, nullptr, nullptr);
}

HWND mkCheck(HWND parent, const wchar_t* text, int x, int y, int w, int id) {
  return CreateWindowExW(0, L"BUTTON", text,
    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, x, y, w, 20, parent,
    (HMENU)(INT_PTR)id, nullptr, nullptr);
}

HWND mkCombo(HWND parent, int x, int y, int w, int id) {
  return CreateWindowExW(0, L"COMBOBOX", nullptr,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
    x, y, w, 200, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
}

// Start one of Koei Tecmo's own front-ends from the game folder.
//
// ARLAND_NO_REDIRECT is set for the child: msimg32.dll sends
// ArlandDXLauncher.exe here in the first place, so without it that button would
// only ever reopen this window. The variable is removed again immediately, so
// it never reaches the game when Start game is pressed afterwards.
bool runStockTool(const char* exeName) {
  if (!g_gameDir[0])
    return false;
  char path[MAX_PATH];
  lstrcpynA(path, g_gameDir, MAX_PATH);
  lstrcatA(path, exeName);
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

// True when the named executable sits in the game folder, so a button that
// opens it can be greyed out rather than failing when pressed.
bool stockToolPresent(const char* exeName) {
  if (!g_gameDir[0])
    return false;
  char path[MAX_PATH];
  lstrcpynA(path, g_gameDir, MAX_PATH);
  lstrcatA(path, exeName);
  return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

HWND mkButton(HWND parent, const wchar_t* text, int x, int y, int w, int id,
              bool isDefault = false) {
  return CreateWindowExW(0, L"BUTTON", text,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP |
      (isDefault ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
    x, y, w, 26, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
}

// The font the running version of Windows actually uses for UI text, which is
// Segoe UI on 10 and 11 and whatever succeeds it later. Asking the OS is the
// only way that stays right across versions; DEFAULT_GUI_FONT is still the
// 1990s bitmap face and is the single biggest reason a plain Win32 window looks
// dated. Falls back to it only if the query fails.
HFONT createUiFont() {
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
      &metrics, 0)) {
    if (HFONT font = CreateFontIndirectW(&metrics.lfMessageFont))
      return font;
  }
  return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
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
HWND mkDesc(HWND parent, const wchar_t* text, int y) {
  // Two lines' worth of height: a static word-wraps on its own, so a longer
  // note reflows instead of being clipped. Rows are pitched to suit.
  HWND h = mkLabel(parent, text, 40, y, 412, 32);
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
  for (int p = 0; p < 3; ++p)
    for (int i = 0; i < g_pageCount[p]; ++i)
      ShowWindow(g_pageCtrls[p][i], p == page ? SW_SHOW : SW_HIDE);
  // The outgoing page's labels leave their text behind them, so the page is
  // redrawn as a whole rather than relying on each control to clean up after
  // itself.
  if (g_hTabs)
    repaintUnder(g_hTabs);
}

void createControls(HWND w) {
  const int L = 24;    // left text column
  const int F = 176;   // field column

  // Which game this folder is, before anything else: the tool configures
  // whatever it sits next to, so that is the first thing to confirm.
  wchar_t heading[160];
  wsprintfW(heading, L"Game: %s", g_gameName ? g_gameName : L"not detected");
  mkLabel(w, heading, 16, 10, 452, 18);

  g_hTabs = CreateWindowExW(0, WC_TABCONTROLW, nullptr,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP, 12, 32, 456, 464,
    w, (HMENU)(INT_PTR)IDC_TABS, nullptr, nullptr);
  TCITEMW tab = {};
  tab.mask = TCIF_TEXT;
  const wchar_t* pageNames[3] = { L"Display", L"Image quality", L"Game" };
  for (int i = 0; i < 3; ++i) {
    tab.pszText = (LPWSTR)pageNames[i];
    SendMessageW(g_hTabs, TCM_INSERTITEMW, i, (LPARAM)&tab);
  }

  // ---------------- page 0: Display ----------------
  onPage(0, mkLabel(w, L"Resolution:", L, 74, 140, 18));
  g_hBase = mkCombo(w, F, 70, 200, IDC_BASE);
  onPage(0, g_hBase);
  unsigned maxW = 0, maxH = 0;
  displayMaximum(&maxW, &maxH);
  for (int i = 0; i < kBaseCount; ++i) {
    // Skip anything the display cannot show. Auto (0x0) always stays.
    if (maxW && maxH && kBaseItems[i].w &&
        (kBaseItems[i].w > maxW || kBaseItems[i].h > maxH))
      continue;
    int idx = (int)SendMessageW(g_hBase, CB_ADDSTRING, 0, (LPARAM)kBaseItems[i].label);
    SendMessageW(g_hBase, CB_SETITEMDATA, idx,
      packRes(kBaseItems[i].w, kBaseItems[i].h));
  }
  onPage(0, mkDesc(w,
    L"What reaches the screen. Also written to the game's own settings.", 94));

  onPage(0, mkLabel(w, L"Window mode:", L, 130, 140, 18));
  g_hWinMode = mkCombo(w, F, 126, 200, IDC_WINMODE);
  onPage(0, g_hWinMode);
  for (int i = 0; i < kWindowModeCount; ++i)
    SendMessageW(g_hWinMode, CB_ADDSTRING, 0, (LPARAM)kWindowModes[i].label);
  onPage(0, mkDesc(w,
    L"Borderless fills the monitor without taking over the display, so "
    L"alt-tab is instant. Also written to the game's own settings.", 150));

  // The games present at the display's refresh rate and the mod does not pace
  // frames, so this is a statement of fact rather than a setting. It is here
  // because "is this capped?" is the question the removed frame-rate limit
  // would otherwise leave open.
  // A note rather than a setting, so it sits on its own row below the controls.
  // mkDesc is two lines tall, so this has to clear the note above it and the
  // group heading below it: rows here are pitched 56 apart and a note occupies
  // 32 of that.
  // Under Proton the obvious question is whether the mod fights the frame limit
  // Steam or the compositor is applying. It cannot: it does not pace frames and
  // passes the game's own presentation interval through untouched, so an
  // outside limit is simply obeyed.
  onPage(0, mkDesc(w,
    runningUnderWine()
      ? L"The game runs at your display's refresh rate, 120 Hz and 144 Hz "
        L"included. The mod does not cap the frame rate, so a limit set by "
        L"Steam or the compositor is respected."
      : L"The game runs at your display's refresh rate, 120 Hz and 144 Hz "
        L"included. The mod does not cap the frame rate.", 190));

  // The stock front-ends are still reachable: this tool replaces them, it does
  // not remove them. Greyed out when the executable is not in this folder.
  onPage(0, mkLabel(w, L"The game's own tools", L, 240, 300, 18));
  HWND openEnv = mkButton(w, L"Settings &editor", 24, 264, 130, IDC_OPENENV);
  HWND openLauncher = mkButton(w, L"&Original launcher", 162, 264, 130,
    IDC_OPENLAUNCHER);
  onPage(0, openEnv);
  onPage(0, openLauncher);
  if (!stockToolPresent("ArlandDXEnv.exe"))
    EnableWindow(openEnv, FALSE);
  if (!stockToolPresent("ArlandDXLauncher.exe"))
    EnableWindow(openLauncher, FALSE);
  onPage(0, mkDesc(w,
    L"Koei Tecmo's own settings editor and launcher, opened as they were "
    L"before this tool was installed.", 296));

  // ---------------- page 1: Image quality ----------------
  onPage(1, mkLabel(w, L"Preset:", L, 74, 140, 18));
  g_hPreset = mkCombo(w, F, 70, 200, IDC_PRESET);
  onPage(1, g_hPreset);
  for (int i = 0; i < kPresetCount; ++i)
    SendMessageW(g_hPreset, CB_ADDSTRING, 0, (LPARAM)kPresets[i].label);
  onPage(1, mkDesc(w, L"Sets the four options below.", 94));

  onPage(1, mkLabel(w, L"Supersampling:", L, 130, 140, 18));
  g_hSS = mkCombo(w, F, 126, 170, IDC_SS);
  onPage(1, g_hSS);
  refillSupersampling();
  onPage(1, mkDesc(w,
    L"Renders higher, then scales down. Sharpest, and the most costly. "
    L"Limited to 8K, which is as far as the engine's own targets stretch.",
    150));

  g_hRendLbl = mkDesc(w, L"Render resolution:", 186);
  onPage(1, g_hRendLbl);

  onPage(1, mkLabel(w, L"Multisampling:", L, 226, 150, 18));
  g_hMsaa = mkCombo(w, F, 222, 170, IDC_MSAA);
  onPage(1, g_hMsaa);
  comboFill(g_hMsaa, kMsaaItems, 4);
  onPage(1, mkDesc(w,
    L"MSAA. Smooths geometry edges; supersampling usually beats it.", 246));

  onPage(1, mkLabel(w, L"Texture sharpness:", L, 282, 150, 18));
  g_hAniso = mkCombo(w, F, 278, 170, IDC_ANISO);
  onPage(1, g_hAniso);
  comboFill(g_hAniso, kAnisoItems, 5);
  onPage(1, mkDesc(w,
    L"Anisotropic filtering. Keeps floors and walls sharp when seen at a "
    L"shallow angle; costs nothing per frame.", 302));

  onPage(1, mkLabel(w, L"Shadow detail:", L, 338, 150, 18));
  g_hShadow = mkCombo(w, F, 334, 170, IDC_SHADOW);
  onPage(1, g_hShadow);
  comboFill(g_hShadow, kShadowItems, 4);
  onPage(1, mkDesc(w,
    L"Larger shadow maps, so sharper shadow edges. Costs video memory.", 358));

  g_hSmaa = mkCheck(w, L"Edge smoothing", L, 394, 300, IDC_SMAA);
  onPage(1, g_hSmaa);
  onPage(1, mkDesc(w,
    L"SMAA post-process. Cheap, and catches edges MSAA cannot.", 414));

  // ---------------- page 2: Game ----------------
  onPage(2, mkLabel(w, L"UI font:", L, 74, 90, 18));
  g_hFont = mkCombo(w, F, 70, 280, IDC_FONT);
  onPage(2, g_hFont);
  comboFill(g_hFont, kFontItems, 3);
  onPage(2, mkDesc(w,
    L"Replaced re-renders every string from a bundled scalable font. "
    L"English builds only; Japanese and Chinese keep the original.", 94));

  onPage(2, mkLabel(w, L"Language:", L, 134, 140, 18));
  g_hLang = mkCombo(w, F, 130, 200, IDC_LANG);
  onPage(2, g_hLang);
  comboFill(g_hLang, kLangItems, kLangCount);
  onPage(2, mkDesc(w,
    L"Written to the game's own settings. Which languages a copy actually "
    L"has depends on the executable it ships with.", 154));

  onPage(2, mkLabel(w, L"Battle", L, 198, 200, 18));
  g_hBShadow = mkCheck(w, L"Character shadows in battle", L, 222, 400, IDC_BSHADOW);
  onPage(2, g_hBShadow);
  onPage(2, mkDesc(w, L"Restores the shadows Rorona is missing while fighting.",
    242));
  g_hBCutInShadow = mkCheck(w, L"Ground shadows during cut-ins", L, 278, 400,
    IDC_BCUTINSHADOW);
  onPage(2, g_hBCutInShadow);
  onPage(2, mkDesc(w, L"Off by default; the vanilla cut-ins have no shadows.",
    298));
  g_hBCutInDim = mkCheck(w, L"Dim the scene during cut-ins", L, 334, 400,
    IDC_BCUTINDIM);
  onPage(2, g_hBCutInDim);
  onPage(2, mkDesc(w, L"Uncheck to hold close-ups at full brightness.", 354));

  onPage(2, mkLabel(w, L"Diagnostics", L, 390, 200, 18));
  g_hVerbose = mkCheck(w, L"Verbose logging", L, 414, 300, IDC_VERBOSE);
  onPage(2, g_hVerbose);
  onPage(2, mkDesc(w,
    L"Extra detail in arland-fix.log. Crash reports are always written.", 434));

  // Bottom left, away from Save/Close so it cannot be hit by accident: saves
  // first, then launches. Disabled when no game was recognised in this folder,
  // since there would be nothing to run.
  //
  // It is also the default button and takes focus at startup: most of the time
  // this window is opened on the way into the game, not to change something, so
  // Enter should start playing. That matters most on a controller or a handheld,
  // where the alternative is driving a cursor across the window.
  g_hStart = mkButton(w, L"&Start game", 24, 508, 110, IDC_START, true);
  if (!g_gameExePath[0])
    EnableWindow(g_hStart, FALSE);

  // Distinct mnemonics across the whole window: S start, A save, C close,
  // E editor, O original launcher.
  mkButton(w, L"S&ave", 280, 508, 90, IDC_SAVE);
  mkButton(w, L"&Close", 378, 508, 90, IDC_CLOSE);

  showPage(0);

  applyFont(w);
}

LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      createControls(w);
      loadFromIni();
      return 0;

    case WM_NOTIFY: {
      const NMHDR* note = (const NMHDR*)lp;
      if (note && note->hwndFrom == g_hTabs && note->code == TCN_SELCHANGE) {
        showPage((int)SendMessageW(g_hTabs, TCM_GETCURSEL, 0, 0));
        return 0;
      }
      break;
    }
    case WM_CTLCOLORSTATIC: {
      // Every label sits on top of the tab control, and a themed tab page is
      // lighter than COLOR_BTNFACE. Handing back a real brush therefore paints
      // that grey behind the text, which reads as a box drawn around each line.
      // A hollow brush leaves the page the tab control has already drawn, and
      // TRANSPARENT stops the text bringing its own background with it. The
      // cost is that a static no longer erases what it had before, which is
      // what repaintUnder is for.
      SetBkMode((HDC)wp, TRANSPARENT);
      // The one-line notes under each control are secondary text, so they are
      // drawn in the system's grey rather than competing with the labels they
      // explain.
      for (int i = 0; i < g_descCount; ++i) {
        if ((HWND)lp == g_hDesc[i]) {
          SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
          break;
        }
      }
      return (LRESULT)GetStockObject(NULL_BRUSH);
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
        case IDC_SAVE: {
          saveToIni();
          // Name the game back to the user: this tool configures whichever
          // folder it sits in, and saying which one closes that loop.
          wchar_t saved[320];
          wsprintfW(saved,
            L"The configuration has been saved successfully. The next time "
            L"you launch %s these settings will be used.",
            g_gameName ? g_gameName : L"the game");
          MessageBoxW(w, saved, L"Atelier Arland Fixes", MB_OK | MB_ICONINFORMATION);
          return 0;
        }
        case IDC_START: {
          // Save first: starting the game with the settings still only on
          // screen is the one outcome nobody wants from this button.
          saveToIni();
          // Which executable to run follows the language that was just saved,
          // exactly as the game's own launcher decides it. Read from the
          // control rather than the file so it is the selection in front of the
          // user, not a stale one.
          char exePath[MAX_PATH] = {};
          const bool have = gameExeForLanguage(
            comboValue(g_hLang, kLangItems, kLangCount), exePath);
          // CreateProcess rather than ShellExecute: the game has to be a child
          // of this process for Steam to keep counting the session as running,
          // which is what keeps the overlay and Steam Input attached to it.
          STARTUPINFOA startup = {};
          startup.cb = sizeof(startup);
          PROCESS_INFORMATION process = {};
          const BOOL started = have && CreateProcessA(exePath, nullptr, nullptr,
            nullptr, FALSE, 0, nullptr, g_gameDir, &startup, &process);
          if (!started) {
            wchar_t failed[320];
            wsprintfW(failed,
              L"The configuration was saved, but %s could not be started "
              L"(error %lu). Launch the game as you normally would; the saved "
              L"settings still apply.",
              g_gameName ? g_gameName : L"the game",
              have ? GetLastError() : (DWORD)ERROR_FILE_NOT_FOUND);
            MessageBoxW(w, failed, L"Atelier Arland Fixes", MB_OK | MB_ICONWARNING);
            return 0;
          }
          CloseHandle(process.hThread);
          CloseHandle(process.hProcess);
          // The game is running and owns the screen from here, so the
          // configurator steps out of the way rather than sitting behind it.
          DestroyWindow(w);
          return 0;
        }
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
        // key. Closing without saving matches every other dialog-shaped window.
        case IDCANCEL:
          DestroyWindow(w);
          return 0;
      }
      return 0;

    case WM_CLOSE:
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
  INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
  InitCommonControlsEx(&icc);

  g_uiFont = createUiFont();

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
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = L"ArlandConfigWindow";
  RegisterClassExW(&wc);

  // Fixed-size dialog-style window (no maximize / resize).
  const DWORD style = (WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME))
                      | WS_VISIBLE;
  RECT r = { 0, 0, 484, 548 };
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
