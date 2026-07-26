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

// Display (backbuffer / present) resolution: [Rendering] DisplayWidth/Height,
// falling back to the legacy Width/Height keys. False when unset, meaning the
// swap chain keeps the size the game itself requested (the old launcher's res).
bool displayResolution(unsigned int* width, unsigned int* height);

// Internal render resolution: [Rendering] RenderWidth/Height, falling back to
// the display resolution. When larger than display the whole frame is
// supersampled down to the display size at present.
bool renderResolution(unsigned int* width, unsigned int* height);

// Backwards-compatible single override used by the current pipeline; resolves
// to the display (backbuffer) resolution. Prefer displayResolution/
// renderResolution in new code.
bool configuredResolution(unsigned int* width, unsigned int* height);

// Borderless windowed mode: [Rendering] Borderless. Runs the game as an
// undecorated window filling its monitor instead of taking exclusive control of
// the display -- instant alt-tab, and friendlier to compositors and multi-
// monitor setups under Wine and Proton. Off by default.
bool borderlessWindow();

// Requested MSAA sample count (1/2/4/8) from ARLAND_MSAA or [Rendering] MSAA.
unsigned int msaaSamples();

// Whether extra diagnostic logging is enabled: [Diagnostics] VerboseLogging
// (default false), or ARLAND_VERBOSE_LOG. Gates the periodic process-memory
// probe and other opt-in diagnostic lines so the default log stays quiet.
bool verboseLogging();

// Stands the whole mod down for one launch. Set by ARLAND_DISABLE, which the
// launcher's "Play without the mod" button passes to the game. See config.cpp.
bool modDisabled();

// Write the settings actually in force to the log once at startup: the INI
// path and uncommented values, relevant environment variables, and effective
// values after resolution clamping.
void logConfiguration();

// How UI text is rendered: [Rendering] Font. "replaced" (the DEFAULT: re-render
// each string from the embedded scalable font (National Park), multi-line and
// glyph-atlas-cached, falling back to upscaling for glyphs it can't resolve),
// "upscaled" (filter-upscale the baked glyphs, preserving the engine's exact
// layout), or "original" (the untouched baked bitmap font; "off" stays accepted
// as a legacy alias). A arland-hires-font.ttf beside the DLL
// overrides the embedded font. ARLAND_UIFONT overrides the mode.
enum class UIFontMode { Original, Upscaled, Replaced };
UIFontMode uiFontMode();

}  // namespace atfix
