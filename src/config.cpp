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
      WritePrivateProfileStringA("Rendering", "Width", "", result.data());
      WritePrivateProfileStringA("Rendering", "Height", "", result.data());
      WritePrivateProfileStringA("Rendering", "ShadowMultiplier", "1", result.data());
      WritePrivateProfileStringA("Battle", "BattleShadows", "true", result.data());
      // The cut-in keys (BattleCutInShadows / BattleCutInDimming) are seeded
      // lazily by featureEnabled() using their per-game matrix defaults, so they
      // stay correct when those defaults change (currently OptIn/off while the
      // cut-in shadow glitch is fixed); not written eagerly here.
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

bool configuredResolution(UINT* width, UINT* height) {
  const char* path = configPath();
  if (!path)
    return false;
  char widthValue[16] = { };
  char heightValue[16] = { };
  GetPrivateProfileStringA("Rendering", "Width", "", widthValue,
    sizeof(widthValue), path);
  GetPrivateProfileStringA("Rendering", "Height", "", heightValue,
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

UIFontMode uiFontMode() {
  static const UIFontMode mode = []() -> UIFontMode {
    char value[16] = { };
    if (const char* env = std::getenv("ARLAND_UIFONT")) {
      std::strncpy(value, env, sizeof(value) - 1);
    } else if (const char* path = configPath()) {
      GetPrivateProfileStringA("Other", "UIFont", "\x01", value,
        sizeof(value), path);
      if (value[0] == '\x01') {                    // absent: seed the default
        WritePrivateProfileStringA("Other", "UIFont", "upscaled", path);
        std::strncpy(value, "upscaled", sizeof(value) - 1);
      }
    } else {
      std::strncpy(value, "upscaled", sizeof(value) - 1);
    }
    if (!_strnicmp(value, "default", 7) || !_strnicmp(value, "off", 3))
      return UIFontMode::Default;
    if (!_strnicmp(value, "replace", 7))
      return UIFontMode::Replaced;   // recognized; not implemented (see TODO)
    return UIFontMode::Upscaled;     // the default
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
