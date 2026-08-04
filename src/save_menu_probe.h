// SPDX-License-Identifier: MIT
//
// Diagnostic for the save data slots view: times the row builder and counts the
// Steam storage helpers. Off unless ARLAND_SAVE_MENU_PROBE=1. See
// save_menu_probe.cpp.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hook_util.h"

namespace atfix {

// Totori English only. Measures; changes no behaviour.
bool installSaveMenuProbe(BYTE* base, const Game& game);

}  // namespace atfix
