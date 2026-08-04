// SPDX-License-Identifier: MIT
//
// Two corrections to field character collision in Atelier Totori: monsters
// snapping across the ground when their AI re-targets, and characters being
// pulled together when they are further apart than their combined radius. See
// field_collision_fix.cpp.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hook_util.h"

namespace atfix {

// Totori English only. Rorona and Meruru rename the field character classes and
// nothing ports by address, so they report not_applicable and install nothing.
bool installFieldCollisionFix(BYTE* base, const Game& game);

}  // namespace atfix
