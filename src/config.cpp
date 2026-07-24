// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "game.h"
#include "log.h"

namespace atfix {

extern Log log;   // lives in main.cpp

const char* configPath() {
  static const std::array<char, MAX_PATH + 1> path = [] {
    std::array<char, MAX_PATH + 1> result = { };
    const DWORD pathLength = GetModuleFileNameA(
      nullptr, result.data(), MAX_PATH);
    if (pathLength && pathLength < MAX_PATH) {
      char* back = std::strrchr(result.data(), '\\');
      char* forward = std::strrchr(result.data(), '/');
      char* slash = !back || (forward && forward > back) ? forward : back;
      if (slash)
        std::memcpy(slash + 1, "arland-fix.ini", sizeof("arland-fix.ini"));
    }

    if (result[0] &&
        GetFileAttributesA(result.data()) == INVALID_FILE_ATTRIBUTES) {
      WritePrivateProfileStringA("Rendering", "MSAA", "1", result.data());
      // Display = backbuffer / panel resolution (blank uses the game's own,
      // i.e. the old launcher's, resolution). Render = internal render target
      // (blank equals Display). A Render larger than Display supersamples the
      // whole frame down to Display at present.
      WritePrivateProfileStringA("Rendering", "DisplayWidth", "", result.data());
      WritePrivateProfileStringA("Rendering", "DisplayHeight", "", result.data());
      WritePrivateProfileStringA("Rendering", "RenderWidth", "", result.data());
      WritePrivateProfileStringA("Rendering", "RenderHeight", "", result.data());
      WritePrivateProfileStringA("Rendering", "ShadowMultiplier", "1", result.data());
      WritePrivateProfileStringA("Battle", "BattleShadows", "true", result.data());
      // The cut-in keys (BattleCutInShadows / BattleCutInDimming) are seeded
      // lazily by featureEnabled() using their per-game matrix defaults, so they
      // stay correct when those defaults change (currently OptIn / off on every
      // supported game, since the cut-in restorations ship opt-in); not written
      // eagerly here.
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

// Shadow-map edge length. Opt-in: only 2048/4096/8192 enlarge the maps; any
// other value (or no config) keeps the vanilla 1024 behaviour byte-identical.
// ARLAND_SHADOW_MULTIPLIER overrides arland-fix.ini [Rendering] ShadowMultiplier;
// like arlandConfigBool, a missing ini key is written back for discovery. The
// multiplier (1, 2, 4 or 8) scales the engine's 1024x1024 shadow map.
UINT shadowMapResolution() {
  static const UINT resolution = []() -> UINT {
    unsigned long multiplier = 1;
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
        WritePrivateProfileStringA("Rendering", "ShadowMultiplier", "1", path);
      else
        multiplier = std::strtoul(iniValue, nullptr, 10);
    }
    if (multiplier == 2 || multiplier == 4 || multiplier == 8) {
      const UINT size = 1024u * static_cast<UINT>(multiplier);
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

bool displayResolution(UINT* width, UINT* height) {
  // New DisplayWidth/Height, falling back to the legacy Width/Height keys.
  // Blank on both means the caller leaves the swap chain at the size the game
  // itself requested (the old launcher's resolution).
  return readResPair("DisplayWidth", "DisplayHeight", width, height) ||
         readResPair("Width", "Height", width, height);
}

bool renderResolution(UINT* width, UINT* height) {
  // The internal render size: RenderWidth/Height, falling back to the display
  // resolution. When larger than display, the frame is supersampled down at
  // present.
  return readResPair("RenderWidth", "RenderHeight", width, height) ||
         displayResolution(width, height);
}

bool configuredResolution(UINT* width, UINT* height) {
  // Backwards-compatible single override still used by the current pipeline for
  // both the swap chain and the main render target. It resolves to the display
  // (backbuffer) size; the render/display split and the supersampling downscale
  // that consume renderResolution() land in a later step, so a lone RenderWidth
  // cannot create a render/backbuffer size mismatch before the downscale exists.
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
    if (!_strnicmp(value, "default", 7) || !_strnicmp(value, "off", 3))
      return UIFontMode::Default;
    if (!_strnicmp(value, "upscale", 7))
      return UIFontMode::Upscaled;
    return UIFontMode::Replaced;     // the default (embedded Cuprum, see font_hires)
  }();
  return mode;
}

EmbeddedFont embeddedFontChoice() {
  static const EmbeddedFont font = []() -> EmbeddedFont {
    char value[24] = { };
    if (const char* env = std::getenv("ARLAND_FONT_NAME")) {
      std::strncpy(value, env, sizeof(value) - 1);
    } else if (const char* path = configPath()) {
      GetPrivateProfileStringA("Rendering", "FontName", "\x01", value,
        sizeof(value), path);
      if (value[0] == '\x01') {                    // absent: seed the default
        WritePrivateProfileStringA("Rendering", "FontName", "NationalPark", path);
        std::strncpy(value, "NationalPark", sizeof(value) - 1);
      }
    } else {
      std::strncpy(value, "NationalPark", sizeof(value) - 1);
    }
    if (!_strnicmp(value, "cuprum", 6))
      return EmbeddedFont::Cuprum;
    return EmbeddedFont::NationalPark;     // the default
  }();
  return font;
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

UINT msaaSamples() {
  static const UINT samples = [] {
    const char* path = configPath();

    char value[16] = { };
    const DWORD length = GetEnvironmentVariableA("ARLAND_MSAA", value, sizeof(value));
    unsigned long requested = 1;
    if (length) {
      requested = std::strtoul(value, nullptr, 10);
    } else if (path) {
      requested = GetPrivateProfileIntA(
        "Rendering", "MSAA", 1, path);
    }
    if (requested < 2)
      return 1u;
    if (requested >= 8)
      return 8u;
    if (requested >= 4)
      return 4u;
    return 2u;
  }();
  return samples;
}

}  // namespace atfix
