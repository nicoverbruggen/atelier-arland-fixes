// SPDX-License-Identifier: MIT
#pragma once
//
// Internal interface between the menu-fix core (menu_fix.cpp) and the
// battle-shadow-restore module (battle_shadow_restore.cpp), which were split out
// of one file. This is not public (the public API is menu_fix.h). It declares the
// two globals shared across the seam and the two battle entry points the menu
// core drives.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>

#include "hook_util.h"   // Game

namespace atfix {

// Defined in menu_fix.cpp (the menu core), read by the battle module.
extern BYTE* gameBase;
extern bool supportedGame;

// Defined in battle_shadow_restore.cpp, called from the menu hook dispatcher and
// the per-frame Present tick.
void installBattleShadowRestore(BYTE* base, const Game& game);
void battleFrameTick();

// Battle-owned (defined in battle_shadow_restore.cpp); the menu core reads it for
// the Present-time diagnostics.
extern std::atomic<bool> g_battleActive;

}  // namespace atfix
