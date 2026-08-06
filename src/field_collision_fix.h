// SPDX-License-Identifier: MIT
//
// Two corrections to field character collision in the Arland games: monsters
// snapping across the ground when their AI re-targets, and characters being
// pulled together when they are further apart than their combined radius. See
// field_collision_fix.cpp.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hook_util.h"

namespace atfix {

// All six executables. A build whose signatures do not match reports failure
// and installs nothing.
bool installFieldCollisionFix(BYTE* base, const Game& game);

}  // namespace atfix
