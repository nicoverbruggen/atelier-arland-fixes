// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cwchar>

#include "config.h"
#include "game.h"
#include "log.h"
#include "path_util.h"

namespace atfix {

extern Log log;   // lives in main.cpp

const char* configPath() {
  static const std::array<char, MAX_PATH + 1> path = [] {
    std::array<char, MAX_PATH + 1> result = { };
    const DWORD pathLength = GetModuleFileNameA(
      nullptr, result.data(), MAX_PATH);
    // GetModuleFileNameA returns MAX_PATH when it had to truncate, and a
    // truncated path is not the directory we want. Swapping the exe name for
    // the ini name has to fit as well. Failing either, the buffer is cleared so
    // this returns nullptr: keeping the exe path would write every setting to a
    // file nothing reads, which fails silently rather than visibly.
    if (!pathLength || pathLength >= MAX_PATH ||
        !replaceFileName(result.data(), result.size(), "arland-fix.ini"))
      result[0] = '\0';

    if (result[0] &&
        GetFileAttributesA(result.data()) == INVALID_FILE_ATTRIBUTES) {
      // Display = backbuffer / panel resolution (blank keeps whatever the
      // game's own settings selected). Render = internal render target (blank
      // equals Display). A Render larger than Display supersamples the whole
      // frame down to Display at present.
      WritePrivateProfileStringA("Rendering", "DisplayWidth", "", result.data());
      WritePrivateProfileStringA("Rendering", "DisplayHeight", "", result.data());
      WritePrivateProfileStringA("Rendering", "RenderWidth", "", result.data());
      WritePrivateProfileStringA("Rendering", "RenderHeight", "", result.data());
      WritePrivateProfileStringA("Rendering", "ShadowMultiplier", "2", result.data());
      // The cut-in keys (BattleCutInShadows / BattleCutInDimming) are seeded
      // lazily by featureEnabled() using their per-game matrix defaults, so they
      // stay correct when those defaults change (currently OptIn on every
      // supported game, so the cut-ins render as the game shipped them); not
      // written eagerly here. Rorona's ordinary-battle shadow restoration has no
      // key at all: it is a fix, not a setting.
    }
    return result;
  }();
  return path[0] ? path.data() : nullptr;
}

// Read a boolean from arland-fix.ini. If the key is missing, write the default
// into the file (so users discover the option) and return it. Accepts
// true/false, 1/0, yes/no (first character, case-insensitive).
bool arlandConfigBool(const char* section, const char* key, bool def) {
  const char* path = configPath();
  if (!path)
    return def;
  char value[16] = { };
  GetPrivateProfileStringA(section, key, "\x01", value, sizeof(value), path);
  if (value[0] == '\x01') {   // absent: persist the default so it appears in the ini
    WritePrivateProfileStringA(section, key, def ? "true" : "false", path);
    return def;
  }
  return value[0] == 't' || value[0] == 'T' || value[0] == '1' ||
         value[0] == 'y' || value[0] == 'Y';
}

// Shadow-map edge length. Only 2048/4096/8192 enlarge the maps; any other value
// keeps the vanilla 1024 behaviour byte-identical, and 1 is how you ask for it.
// ARLAND_SHADOW_MULTIPLIER overrides arland-fix.ini [Rendering] ShadowMultiplier;
// like arlandConfigBool, a missing ini key is written back for discovery. The
// multiplier (1, 2, 4 or 8) scales the engine's 1024x1024 shadow map.
//
// Defaults to 2. The engine draws every shadow in a scene into one 1024 map, so
// at the resolutions this mod renders at the edges are visibly blocky; 2048
// costs little video memory and is what stops it looking broken. Both seed
// sites have to agree: this one and the file creation in configPath() above.
UINT shadowMapResolution() {
  static const UINT resolution = []() -> UINT {
    unsigned long multiplier = 2;
    char value[16] = { };
    const DWORD length = GetEnvironmentVariableA(
      "ARLAND_SHADOW_MULTIPLIER", value, sizeof(value));
    if (length && length < sizeof(value)) {
      multiplier = std::strtoul(value, nullptr, 10);
    } else if (const char* path = configPath()) {
      char iniValue[16] = { };
      GetPrivateProfileStringA("Rendering", "ShadowMultiplier", "\x01",
        iniValue, sizeof(iniValue), path);
      if (iniValue[0] == '\x01')
        WritePrivateProfileStringA("Rendering", "ShadowMultiplier", "2", path);
      else
        multiplier = std::strtoul(iniValue, nullptr, 10);
    }
    if (multiplier == 2 || multiplier == 4 || multiplier == 8) {
      const UINT size = 1024u * static_cast<UINT>(multiplier);
      if (verboseLogging())
        log("Shadow-map resolution override: ", std::dec, size, "x", size,
          " (", multiplier, "x)");
      return size;
    }
    return 1024u;
  }();
  return resolution;
}

// Read a [Rendering] resolution pair under the given key names. Returns false
// unless both parse and land in a sane range (640x360 .. 16384x16384).
static bool readResPair(const char* widthKey, const char* heightKey,
                        UINT* width, UINT* height) {
  const char* path = configPath();
  if (!path)
    return false;
  char widthValue[16] = { };
  char heightValue[16] = { };
  GetPrivateProfileStringA("Rendering", widthKey, "", widthValue,
    sizeof(widthValue), path);
  GetPrivateProfileStringA("Rendering", heightKey, "", heightValue,
    sizeof(heightValue), path);
  const unsigned long parsedWidth = std::strtoul(widthValue, nullptr, 10);
  const unsigned long parsedHeight = std::strtoul(heightValue, nullptr, 10);
  if (parsedWidth < 640 || parsedWidth > 16384 ||
      parsedHeight < 360 || parsedHeight > 16384)
    return false;
  *width = static_cast<UINT>(parsedWidth);
  *height = static_cast<UINT>(parsedHeight);
  return true;
}

// The largest mode the display reports. Presenting larger than the panel gains
// nothing -- the extra pixels are scaled away again -- so a display resolution
// above this is a misunderstanding of what the key does; RenderWidth/Height is
// how you render higher than you present. Enumerated once; falls back to the
// current desktop size if enumeration fails.
static bool displayMaximum(UINT* width, UINT* height) {
  struct Maximum { UINT width; UINT height; };
  static const Maximum maximum = [] {
    Maximum best { 0, 0 };
    DEVMODEA mode = { };
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsA(nullptr, i, &mode); ++i) {
      if (uint64_t(mode.dmPelsWidth) * mode.dmPelsHeight >
          uint64_t(best.width) * best.height)
        best = { mode.dmPelsWidth, mode.dmPelsHeight };
    }
    if (!best.width || !best.height) {
      const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
      const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
      if (screenWidth > 0 && screenHeight > 0)
        best = { UINT(screenWidth), UINT(screenHeight) };
    }
    return best;
  }();
  if (!maximum.width || !maximum.height)
    return false;
  *width = maximum.width;
  *height = maximum.height;
  return true;
}

// The resolution the desktop is running at now. Deliberately not
// displayMaximum(): a panel that supports 4K but runs its desktop at 1440p would
// otherwise be handed 4K and have it scaled straight back down. Resolved once,
// so the answer stays stable for the process.
static bool displayCurrent(UINT* width, UINT* height) {
  struct Current { UINT width; UINT height; };
  static const Current current = [] {
    DEVMODEA mode = { };
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &mode) &&
        mode.dmPelsWidth && mode.dmPelsHeight)
      return Current { mode.dmPelsWidth, mode.dmPelsHeight };
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    if (screenWidth > 0 && screenHeight > 0)
      return Current { UINT(screenWidth), UINT(screenHeight) };
    return Current { 0, 0 };
  }();
  if (!current.width || !current.height)
    return false;
  *width = current.width;
  *height = current.height;
  return true;
}

bool displayResolution(UINT* width, UINT* height) {
  if (!readResPair("DisplayWidth", "DisplayHeight", width, height)) {
    // Blank, half-filled or unparseable presents at the desktop resolution. The
    // game's own default is 720p, so following it made a fresh install look far
    // worse than the screen it is running on for anyone who never opened the
    // launcher. Failing here means the desktop mode could not be read at all,
    // and the caller leaves the game's own choice alone.
    return displayCurrent(width, height);
  }

  UINT maxWidth = 0;
  UINT maxHeight = 0;
  if (displayMaximum(&maxWidth, &maxHeight) &&
      (*width > maxWidth || *height > maxHeight)) {
    static std::atomic<bool> warned { false };
    if (!warned.exchange(true))
      log("Display resolution ", std::dec, *width, "x", *height,
        " exceeds the display's ", maxWidth, "x", maxHeight,
        "; using the display's instead. To render at a higher resolution than"
        " the screen, set RenderWidth/RenderHeight -- that is supersampling,"
        " and it is downscaled to the display resolution at present.");
    *width = maxWidth;
    *height = maxHeight;
  }
  return true;
}

bool renderResolution(UINT* width, UINT* height) {
  // The internal render size: RenderWidth/Height, falling back to the display
  // resolution. When larger than display, the frame is supersampled down at
  // present.
  if (!readResPair("RenderWidth", "RenderHeight", width, height))
    return displayResolution(width, height);

  // 8K is where this stops buying anything. These are 2010-era assets: past
  // roughly 8K there is no sub-pixel detail left for more samples to resolve,
  // while the cost keeps scaling with pixels drawn -- the engine derives a
  // large family of render targets from this size, and a 4K panel at 4x asks
  // for over half a gigabyte each. So the limit is diminishing returns first
  // and memory second. The ratio is kept, so the frame stays the shape the
  // display asked for.
  //
  // Nothing structural stops this being raised later if a reason appears; it
  // is a judgement about these games, not a constraint of the code. Note the
  // black conversations that once looked like evidence for a memory ceiling
  // were a render-target classification bug (see sync_fix.cpp, the 1920x1080
  // full-size vs half-size tie-break) and are NOT an argument for this limit.
  const UINT maxWidth = 7680;
  const UINT maxHeight = 4320;
  if (*width > maxWidth || *height > maxHeight) {
    static std::atomic<bool> warned { false };
    const UINT clampedWidth = *width * maxHeight >= *height * maxWidth
      ? maxWidth : UINT(uint64_t(*width) * maxHeight / *height);
    const UINT clampedHeight = *width * maxHeight >= *height * maxWidth
      ? UINT(uint64_t(*height) * maxWidth / *width) : maxHeight;
    if (!warned.exchange(true))
      log("Render resolution ", std::dec, *width, "x", *height,
        " exceeds the ", maxWidth, "x", maxHeight, " supersampling limit;"
        " rendering at ", clampedWidth, "x", clampedHeight, " instead.");
    *width = clampedWidth;
    *height = clampedHeight;
  }
  return true;
}

bool configuredResolution(UINT* width, UINT* height) {
  // Backwards-compatible single override, resolving to the display (backbuffer)
  // size. The render/display split is live: the swap chain follows this, while
  // the main render target and everything the engine derives from it follow
  // renderResolution(), and supersample.cpp downscales one into the other at
  // present. With no render resolution configured the two are equal and this is
  // the pre-split behaviour.
  return displayResolution(width, height);
}

UIFontMode uiFontMode() {
  static const UIFontMode mode = []() -> UIFontMode {
    char value[16] = { };
    if (const char* env = std::getenv("ARLAND_UIFONT")) {
      std::strncpy(value, env, sizeof(value) - 1);
    } else if (const char* path = configPath()) {
      GetPrivateProfileStringA("Rendering", "Font", "\x01", value,
        sizeof(value), path);
      if (value[0] == '\x01') {                    // absent: seed the default
        WritePrivateProfileStringA("Rendering", "Font", "replaced", path);
        std::strncpy(value, "replaced", sizeof(value) - 1);
      }
    } else {
      std::strncpy(value, "replaced", sizeof(value) - 1);
    }
    if (!_strnicmp(value, "original", 8) || !_strnicmp(value, "off", 3))
      return UIFontMode::Original;   // "off" is a synonym for "original"
    if (!_strnicmp(value, "upscale", 7))
      return UIFontMode::Upscaled;
    return UIFontMode::Replaced;     // the default (embedded National Park)
  }();
  return mode;
}

bool verboseLogging() {
  static const bool on = [] {
    const char* env = std::getenv("ARLAND_VERBOSE_LOG");
    if (env)
      return env[0] != '0';
    return arlandConfigBool("Diagnostics", "VerboseLogging", false);
  }();
  return on;
}

// Stands the whole mod down: it still loads, and still forwards Direct3D,
// because the game imports this DLL by name and would not start otherwise --
// but it installs no hooks, detours nothing, and overrides no resolution. The
// result is the game as it ships.
//
// This exists so that "is the mod causing this?" can be answered in one launch,
// without moving files out of the game folder and forgetting to move them back.
// Environment only, and deliberately not an ini key: it is a property of a
// single launch rather than a setting, and a disabled mod that stayed disabled
// across launches because of a line in a file would be a trap.
bool modDisabled() {
  static const bool disabled = [] {
    const char* value = std::getenv("ARLAND_DISABLE");
    return value && value[0] != '0';
  }();
  return disabled;
}

// ---- the configuration block in the log ------------------------------------
//
// Every setting that shaped a run, written once at startup. This exists because
// a log without it cannot be diagnosed alone: a report of "supersampling does
// nothing" is indistinguishable from "supersampling was never configured"
// unless the log says which. The file is short, comments are stripped, and only
// relevant environment variables are included.
//
//   CONFIG ini   -- the file as it reads on disk, comments stripped
//   CONFIG env   -- every ARLAND_* variable, which override the ini
//   CONFIG using -- the resolutions after clamping, which is what actually ran
void logIniFile() {
  const char* path = configPath();
  if (!path) {
    log("CONFIG no arland-fix.ini path could be resolved");
    return;
  }
  log("CONFIG source ", path);
  HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    log("CONFIG the ini could not be opened; every value below is a default");
    return;
  }
  std::string text;
  char chunk[4096];
  DWORD read = 0;
  while (ReadFile(file, chunk, sizeof(chunk), &read, nullptr) && read)
    text.append(chunk, read);
  CloseHandle(file);

  // Comments and blank lines are the bulk of the shipped file and say nothing
  // about this run, so only real content is kept.
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos)
      end = text.size();
    std::string line = text.substr(start, end - start);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line[first] != '#' && line[first] != ';')
      log("CONFIG ini   ", line.c_str() + first);
    if (end == text.size())
      break;
    start = end + 1;
  }
}

void logEnvironmentOverrides() {
  wchar_t* block = GetEnvironmentStringsW();
  if (!block)
    return;
  // A short allowlist of Steam's own markers alongside our variables. These say
  // how the game was started -- Big Picture, a Deck, the desktop client -- which
  // is not visible anywhere else and decides whether the launcher should offer a
  // controller-friendly layout. Named explicitly rather than by a Steam* prefix
  // because the rest of that namespace carries the account name and install
  // paths, and these logs get sent to us.
  static const wchar_t* const kSteamMarkers[] = {
    L"SteamTenfoot", L"SteamDeck", L"SteamOS", L"SteamClientLaunch",
    L"SteamGameId", L"SteamAppId",
  };

  int found = 0;
  for (const wchar_t* entry = block; *entry; entry += std::wcslen(entry) + 1) {
    bool wanted = _wcsnicmp(entry, L"ARLAND_", 7) == 0;
    for (const wchar_t* marker : kSteamMarkers) {
      const std::size_t length = std::wcslen(marker);
      if (_wcsnicmp(entry, marker, length) == 0 && entry[length] == L'=')
        wanted = true;
    }
    if (!wanted)
      continue;
    char narrow[512] = { };
    WideCharToMultiByte(CP_UTF8, 0, entry, -1, narrow, sizeof(narrow) - 1,
      nullptr, nullptr);
    log("CONFIG env   ", narrow);
    ++found;
  }
  FreeEnvironmentStringsW(block);
  if (!found)
    log("CONFIG env   (none)");
}

void logConfiguration() {
  static std::atomic<bool> written { false };
  if (written.exchange(true))
    return;

  logIniFile();
  logEnvironmentOverrides();

  // What the mod resolved those inputs to. The resolutions are the ones worth
  // stating outright, because both are clamped -- display to the panel, render
  // to 8K -- so the number that ran is not always the number in the file.
  UINT displayWidth = 0, displayHeight = 0;
  UINT renderWidth = 0, renderHeight = 0;
  const bool haveDisplay = displayResolution(&displayWidth, &displayHeight);
  const bool haveRender = renderResolution(&renderWidth, &renderHeight);
  if (haveDisplay)
    log("CONFIG using display ", std::dec, displayWidth, "x", displayHeight);
  else
    log("CONFIG using display (the game's own, no override)");
  if (haveRender && haveDisplay &&
      (renderWidth > displayWidth || renderHeight > displayHeight))
    log("CONFIG using render ", std::dec, renderWidth, "x", renderHeight,
        " (supersampling)");
  else
    log("CONFIG using render (same as display, no supersampling)");
  const UIFontMode font = uiFontMode();
  log("CONFIG using shadowmap=", std::dec, shadowMapResolution(),
      " font=", font == UIFontMode::Original ? "original"
              : font == UIFontMode::Upscaled ? "upscaled" : "replaced",
      " verbose=", verboseLogging() ? 1 : 0,
      " disabled=", modDisabled() ? 1 : 0);
}

}  // namespace atfix

namespace atfix {

DebugView debugView() {
  static const DebugView view = [] {
    char value[24] = { };
    // The SMAA-specific switch predates the unified view and stays honoured.
    DWORD length = GetEnvironmentVariableA(
      "ARLAND_SMAA_DEBUG", value, sizeof(value));
    if (length && length < sizeof(value)) {
      if (value[0] == '1') return DebugView::SmaaEdges;
      if (value[0] == '2') return DebugView::SmaaWeights;
    }
    length = GetEnvironmentVariableA(
      "ARLAND_DEBUG_VIEW", value, sizeof(value));
    if (!(length && length < sizeof(value))) {
      value[0] = '\0';
      if (const char* path = configPath())
        GetPrivateProfileStringA("Debug", "View", "off",
          value, sizeof(value), path);
    }
    if (!_stricmp(value, "wireframe"))    return DebugView::Wireframe;
    if (!_stricmp(value, "smaa-edges"))   return DebugView::SmaaEdges;
    if (!_stricmp(value, "smaa-weights")) return DebugView::SmaaWeights;
    if (!_stricmp(value, "scene-target")) return DebugView::SceneTarget;
    return DebugView::None;
  }();
  return view;
}

int smaaDebugLevel() {
  switch (debugView()) {
    case DebugView::SmaaEdges:   return 1;
    case DebugView::SmaaWeights: return 2;
    default:                     return 0;
  }
}

bool debugWireframe() {
  return debugView() == DebugView::Wireframe;
}

bool debugSceneTargetHighlight() {
  return debugView() == DebugView::SceneTarget;
}

}  // namespace atfix
