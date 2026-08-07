// SPDX-License-Identifier: MIT
//
// Experimental: removes the hardcoded waits in front of the save data slots
// view. Off unless ARLAND_SAVE_MENU_GATES=1. See fast_save_menu.cpp.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hook_util.h"

namespace atfix {

// Totori only for now, both builds. The other four builds are mapped but not
// wired until this has been validated in play.
bool installSaveMenuFix(BYTE* base, const Game& game);

}  // namespace atfix
