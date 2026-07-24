// SPDX-License-Identifier: MIT
//
// arland-config.exe: a small standalone Win32 GUI to view and edit the
// atelier-arland-fixes mod's arland-fix.ini. It reads and writes the same keys
// the DLL parses in src/config.cpp (and SMAA in smaa.cpp, AnisotropicFiltering
// in sync_fix.cpp), using the exact same GetPrivateProfileStringA /
// WritePrivateProfileStringA API so the on-disk format matches the mod. On save
// it touches only the known keys, so any other or legacy keys already in the
// file are preserved.
//
// No external dependencies: plain Win32 common controls, GUI subsystem.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
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
  IDC_BSHADOW,
  IDC_BCUTINSHADOW,
  IDC_BCUTINDIM,
  IDC_VERBOSE,
  IDC_PATH,
  IDC_BROWSE,
  IDC_SAVE,
  IDC_CLOSE,
};

// Absolute path to the arland-fix.ini we edit. Resolved at startup, changeable
// via Browse.
char g_iniPath[MAX_PATH] = {};

// Handles to the controls we read from and write to.
HWND g_hFont, g_hBase, g_hSS, g_hRendLbl, g_hMsaa, g_hShadow,
     g_hAniso, g_hSmaa, g_hBShadow, g_hBCutInShadow, g_hBCutInDim, g_hVerbose,
     g_hPath;

HFONT g_uiFont = nullptr;

// The dropdown entries. The first column is the label shown to the user, the
// second is the exact string written to the ini.
struct ComboItem { const wchar_t* label; const char* value; };

const ComboItem kFontItems[] = {
  { L"replaced (embedded scalable font, default)", "replaced" },
  { L"upscaled (smooth the original glyphs)",      "upscaled" },
  { L"original (untouched bitmap font)",           "original" },
};
// MSAA / ShadowMultiplier share the same 1/2/4/8 scale (1 = off).
const ComboItem kMsaaItems[] = {
  { L"1 (off)", "1" }, { L"2", "2" }, { L"4", "4" }, { L"8", "8" },
};
const ComboItem kShadowItems[] = {
  { L"1 (off, 1024)", "1" }, { L"2 (2048)", "2" },
  { L"4 (4096)", "4" },      { L"8 (8192)", "8" },
};
const ComboItem kAnisoItems[] = {
  { L"1 (off)", "1" }, { L"2", "2" }, { L"4", "4" },
  { L"8", "8" },       { L"16", "16" },
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
const unsigned kMaxDim = 16384;

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

// The selected supersampling multiplier, or 0.0 when "Off" (index 0).
double selectedMult() {
  int sel = (int)SendMessageW(g_hSS, CB_GETCURSEL, 0, 0);
  if (sel <= 0 || sel >= kSSCount) return 0.0;
  return kSSItems[sel].mult;
}

// Compute the render resolution (base x multiplier, clamped) into out. Returns
// false when there is nothing to render at (Auto base or Off).
bool computeRender(unsigned* rw, unsigned* rh) {
  unsigned bw, bh;
  double m = selectedMult();
  if (m <= 1.0 || !selectedBase(&bw, &bh))
    return false;
  unsigned long long w = (unsigned long long)(bw * m + 0.5);
  unsigned long long h = (unsigned long long)(bh * m + 0.5);
  if (w > kMaxDim) w = kMaxDim;
  if (h > kMaxDim) h = kMaxDim;
  *rw = (unsigned)w; *rh = (unsigned)h;
  return true;
}

// Refresh the live render-resolution label and the enabled state of the
// supersampling dropdown. Supersampling needs a concrete base to compute
// against, so it is greyed out and forced to Off while the base is "Auto".
void updateRenderResolution() {
  unsigned bw, bh;
  bool haveBase = selectedBase(&bw, &bh);
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
  SendMessageW(g_hSS, CB_SETCURSEL, ssSel, 0);

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

  SetWindowTextA(g_hPath, g_iniPath);
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
    wsprintfA(num, "%u", bh);
    WritePrivateProfileStringA("Rendering", "DisplayHeight", num, g_iniPath);
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

  iniWriteBool("Battle", "BattleShadows", isChecked(g_hBShadow));
  iniWriteBool("Battle", "BattleCutInShadows", isChecked(g_hBCutInShadow));
  iniWriteBool("Battle", "BattleCutInDimming", isChecked(g_hBCutInDim));

  iniWriteBool("Diagnostics", "VerboseLogging", isChecked(g_hVerbose));

  // Flush the cache so the file is on disk before we report success.
  WritePrivateProfileStringA(nullptr, nullptr, nullptr, g_iniPath);
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

// Resolve the initial ini path: prefer one beside this exe, else one in the
// current working directory, else fall back to beside the exe (Save will
// create it).
void resolveInitialPath() {
  char exe[MAX_PATH] = {};
  DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
  if (n && n < MAX_PATH) {
    char* slash = std::strrchr(exe, '\\');
    if (slash) {
      slash[1] = '\0';
      iniPathInDir(exe);
      if (GetFileAttributesA(g_iniPath) != INVALID_FILE_ATTRIBUTES)
        return;  // found beside the exe
    }
  }

  char cwd[MAX_PATH] = {};
  if (GetCurrentDirectoryA(MAX_PATH, cwd)) {
    char candidate[MAX_PATH];
    char saved[MAX_PATH];
    lstrcpynA(saved, g_iniPath, MAX_PATH);
    iniPathInDir(cwd);
    lstrcpynA(candidate, g_iniPath, MAX_PATH);
    if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES)
      return;  // found in the working directory
    // Neither exists: keep the beside-the-exe path if we had one.
    if (saved[0]) lstrcpynA(g_iniPath, saved, MAX_PATH);
  }
}

// Let the user pick a game folder; we edit the arland-fix.ini inside it.
void browseForFolder(HWND owner) {
  BROWSEINFOA bi = {};
  bi.hwndOwner = owner;
  bi.lpszTitle = "Select the game folder that holds arland-fix.ini "
                 "(next to d3d11.dll):";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  LPITEMIDLIST idl = SHBrowseForFolderA(&bi);
  if (!idl)
    return;
  char folder[MAX_PATH] = {};
  if (SHGetPathFromIDListA(idl, folder)) {
    iniPathInDir(folder);
    loadFromIni();
  }
  CoTaskMemFree(idl);
}

// ---- window construction ---------------------------------------------------

HWND mkLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
  return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
    x, y, w, h, parent, nullptr, nullptr, nullptr);
}

HWND mkGroup(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
  return CreateWindowExW(0, L"BUTTON", text,
    WS_CHILD | WS_VISIBLE | BS_GROUPBOX, x, y, w, h, parent, nullptr,
    nullptr, nullptr);
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

HWND mkButton(HWND parent, const wchar_t* text, int x, int y, int w, int id) {
  return CreateWindowExW(0, L"BUTTON", text,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x, y, w, 26, parent,
    (HMENU)(INT_PTR)id, nullptr, nullptr);
}

void applyFont(HWND parent) {
  // Use the standard shell dialog font on every child.
  for (HWND c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
    SendMessageW(c, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
}

void createControls(HWND w) {
  const int L = 24;    // left text column inside a group
  const int F = 176;   // field column

  // ---------------- Rendering ----------------
  mkGroup(w, L"Rendering", 12, 6, 456, 316);

  mkLabel(w, L"UI font:", L, 34, 90, 18);
  g_hFont = mkCombo(w, F, 30, 280, IDC_FONT);
  comboFill(g_hFont, kFontItems, 3);

  mkLabel(w, L"Base resolution:", L, 66, 140, 18);
  g_hBase = mkCombo(w, F, 62, 200, IDC_BASE);
  unsigned maxW = 0, maxH = 0;
  displayMaximum(&maxW, &maxH);
  for (int i = 0; i < kBaseCount; ++i) {
    // Skip anything the display cannot actually show. Auto (0x0) always stays.
    if (maxW && maxH && kBaseItems[i].w &&
        (kBaseItems[i].w > maxW || kBaseItems[i].h > maxH))
      continue;
    int idx = (int)SendMessageW(g_hBase, CB_ADDSTRING, 0, (LPARAM)kBaseItems[i].label);
    SendMessageW(g_hBase, CB_SETITEMDATA, idx,
      packRes(kBaseItems[i].w, kBaseItems[i].h));
  }

  mkLabel(w, L"Supersampling:", L, 96, 140, 18);
  g_hSS = mkCombo(w, F, 92, 110, IDC_SS);
  for (int i = 0; i < kSSCount; ++i)
    SendMessageW(g_hSS, CB_ADDSTRING, 0, (LPARAM)kSSItems[i].label);

  g_hRendLbl = mkLabel(w, L"Render resolution:", L, 124, 432, 18);

  mkLabel(w,
    L"Base \"Auto\" keeps the launcher's resolution and disables "
    L"supersampling (no concrete size to scale). Render larger than base "
    L"supersamples the whole frame down to base size at present.",
    L, 146, 432, 32);

  mkLabel(w, L"MSAA:", L, 184, 130, 18);
  g_hMsaa = mkCombo(w, F, 180, 110, IDC_MSAA);
  comboFill(g_hMsaa, kMsaaItems, 4);

  mkLabel(w, L"Shadow resolution:", L, 214, 140, 18);
  g_hShadow = mkCombo(w, F, 210, 110, IDC_SHADOW);
  comboFill(g_hShadow, kShadowItems, 4);

  mkLabel(w, L"Anisotropic filtering:", L, 244, 150, 18);
  g_hAniso = mkCombo(w, F, 240, 110, IDC_ANISO);
  comboFill(g_hAniso, kAnisoItems, 5);

  g_hSmaa = mkCheck(w, L"SMAA (post-process anti-aliasing)", L, 272, 300, IDC_SMAA);

  // ---------------- Battle ----------------
  mkGroup(w, L"Battle", 12, 330, 456, 116);
  g_hBShadow = mkCheck(w,
    L"Battle shadows (restore character shadows in battle)",
    L, 354, 420, IDC_BSHADOW);
  g_hBCutInShadow = mkCheck(w,
    L"Battle cut-in shadows (ground shadows during action cut-ins)",
    L, 380, 420, IDC_BCUTINSHADOW);
  g_hBCutInDim = mkCheck(w,
    L"Battle cut-in dimming (checked = keep dimming; uncheck = full brightness)",
    L, 406, 440, IDC_BCUTINDIM);

  // ---------------- Diagnostics ----------------
  mkGroup(w, L"Diagnostics", 12, 454, 456, 56);
  g_hVerbose = mkCheck(w, L"Verbose logging", L, 476, 300, IDC_VERBOSE);

  // ---------------- ini path + buttons ----------------
  g_hPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
    WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
    12, 522, 340, 22, w, (HMENU)(INT_PTR)IDC_PATH, nullptr, nullptr);
  mkButton(w, L"Browse...", 360, 520, 108, IDC_BROWSE);

  mkButton(w, L"Save", 280, 556, 90, IDC_SAVE);
  mkButton(w, L"Close", 378, 556, 90, IDC_CLOSE);

  applyFont(w);
}

LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      createControls(w);
      loadFromIni();
      return 0;

    case WM_COMMAND:
      // Base or supersampling changed: recompute the render label and the
      // Auto-disables-supersampling rule live.
      if (HIWORD(wp) == CBN_SELCHANGE &&
          (LOWORD(wp) == IDC_BASE || LOWORD(wp) == IDC_SS)) {
        updateRenderResolution();
        return 0;
      }
      switch (LOWORD(wp)) {
        case IDC_BROWSE:
          browseForFolder(w);
          return 0;
        case IDC_SAVE:
          saveToIni();
          MessageBoxA(w, "Settings saved to arland-fix.ini.\n"
            "Close the game before it reads the file.", "arland-config",
            MB_OK | MB_ICONINFORMATION);
          return 0;
        case IDC_CLOSE:
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

  g_uiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  resolveInitialPath();

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
  RECT r = { 0, 0, 484, 596 };
  AdjustWindowRect(&r, style, FALSE);
  HWND w = CreateWindowExW(0, wc.lpszClassName,
    L"Atelier Arland Fixes - Configuration", style,
    CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
    nullptr, nullptr, hInst, nullptr);
  if (!w)
    return 1;

  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(w, &m)) {   // Tab / arrow navigation between controls
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  }
  return (int)m.wParam;
}
