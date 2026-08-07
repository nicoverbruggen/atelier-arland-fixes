// SPDX-License-Identifier: MIT
//
// Removes the hardcoded waits in front of the save data slots view and the one
// on the way out. [Menus] FastSaveMenu, on by default in all three games;
// ARLAND_SAVE_MENU_GATES=0 restores the waits. See fast_save_menu.cpp.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hook_util.h"

namespace atfix {

// All three games, both builds each. Rorona and Meruru carry a fifth gate that
// Totori lacks.
bool installSaveMenuFix(BYTE* base, const Game& game);

}  // namespace atfix
