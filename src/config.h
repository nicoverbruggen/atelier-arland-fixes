// SPDX-License-Identifier: MIT
#pragma once

// arland-fix.ini location and the values read from it. Definitions in
// config.cpp. Types use plain `unsigned int` (== the Win32 UINT the callers
// use) so this header needs no Windows or D3D includes.
namespace atfix {

// Absolute path to arland-fix.ini beside the game executable, creating the file
// with default sections on first use. Null if the path cannot be resolved.
const char* configPath();

// Read a boolean from arland-fix.ini, seeding the default key when it is absent
// so the option is discoverable. Accepts true/false, 1/0, yes/no.
bool arlandConfigBool(const char* section, const char* key, bool def);

// Shadow-map edge length: 1024 by default, or 2048/4096/8192 when the
// [Rendering] ShadowMultiplier (2/4/8) opts in. See ARLAND_SHADOW_MULTIPLIER.
unsigned int shadowMapResolution();

// [Rendering] Width/Height override, if both are present and in range.
bool configuredResolution(unsigned int* width, unsigned int* height);

// Requested MSAA sample count (1/2/4/8) from ARLAND_MSAA or [Rendering] MSAA.
unsigned int msaaSamples();

// Whether extra diagnostic logging is enabled: [Diagnostics] VerboseLogging
// (default false), or ARLAND_VERBOSE_LOG. Gates the periodic process-memory
// probe and other opt-in diagnostic lines so the default log stays quiet.
bool verboseLogging();

// How UI text is rendered: [Rendering] Font — "replaced" (the DEFAULT: re-render
// each string from the embedded scalable font (Cuprum), multi-line and glyph-
// atlas-cached, falling back to upscaling for glyphs it can't resolve), "upscaled"
// (filter-upscale the baked glyphs, preserving the engine's exact layout), or
// "default"/"off" (the untouched baked bitmap font). A arland-hires-font.ttf
// beside the DLL overrides the embedded font. ARLAND_UIFONT overrides the mode.
enum class UIFontMode { Default, Upscaled, Replaced };
UIFontMode uiFontMode();

// Which bundled replacement font "replaced" mode uses when no loose
// arland-hires-font.ttf overrides it: [Rendering] FontName -- "NationalPark"
// (the DEFAULT) or "Cuprum". Both are OFL and compiled into the DLL.
// ARLAND_FONT_NAME overrides the ini key.
enum class EmbeddedFont { NationalPark, Cuprum };
EmbeddedFont embeddedFontChoice();

}  // namespace atfix
