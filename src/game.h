// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

// Per-game capability layer. This header plus the matrix in game.cpp are the
// single source of truth for which enhancements each Arland game receives and
// whether they are on by default; the "Feature support by game" table in
// README.md mirrors the matrix. Both the D3D11 layer and the menu/engine layer
// consult this so a feature can be gated per game without threading the game
// identity through every call site.
namespace atfix {

// Which Arland game we run in, detected from the executable name
// (A11R -> Rorona, A12V -> Totori, A13V -> Meruru). Resolved once, and
// independently of whether the menu hooks install, so the D3D11 layer can gate
// on it even with ARLAND_MENU_FIX=0.
enum class Title : uint8_t { Unknown, Rorona, Totori, Meruru };
Title currentTitle();
const char* titleName(Title t);

// Enhancements the mod can apply. Order must match the matrix columns and the
// descriptor rows in game.cpp.
enum class Feature : uint8_t {
  SyncFix,             // text-safe D3D11 stall reduction (core)
  MenuHitchFix,        // .PSSG cache + menu construction hooks (core)
  AtlasCache,          // atlas read caching
  FrameAtlasCache,     // Rorona-only frame-scoped atlas snapshot cache
  ResolutionOverride,  // direct 1440p/2160p rendering
  Msaa,                // optional multisample AA (valued: sample count)
  ShadowMultiplier,    // higher-resolution shadow map (valued: 1/2/4/8)
  BattleShadows,       // restored ordinary-battle caster shadows
  CutInShadows,        // reopen the reception gate during attack cut-ins
  CutInDimHold,        // hold scene light up during cut-ins (prevent dimming)
  CutsceneFramerate,   // re-assert 60 fps after cutscenes that stick at 30
  Count,
};

// How a feature relates to the current game.
enum class Support : uint8_t {
  Unsupported,   // inapplicable here; env/ini cannot force it on
  OptIn,         // available but off unless the user enables it
  OnByDefault,   // on unless the user disables it
};

// Per-game support/default for a feature (the capability matrix).
Support featureSupport(Feature f);

// Resolved on/off for the current game. Precedence: environment override, then
// the ini key, then the matrix default. Unsupported is a hard off that neither
// env nor ini can turn on. Valued knobs (MSAA, ShadowMultiplier) are read by
// their dedicated readers, not through this; here they only carry a matrix cell
// for the documentation table.
bool featureEnabled(Feature f);

}  // namespace atfix
