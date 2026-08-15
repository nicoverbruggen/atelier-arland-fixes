// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../../core/hook_util.h"

// Frame-rate-independent travel-map analog cursor movement.
//
// Totori and Meruru each have a mover that reads axes 0 and 1, folds in four
// digital directions, rotates that direction by the map heading, and adds the
// resulting normalized vector directly to the position at self+0x30. The
// position addition has no dt term. Each mover's immediate caller does receive
// the real frame dt, but the mover never consumes it. That is exactly the
// reported bug shape: a fixed distance per rendered frame.
//
// The fix rescales that step by min(dt * 60, 1), preserving the original
// behavior at 60 fps and below while making higher refresh rates cover the same
// distance per second. ARLAND_WORLDMAP_FIX=0 disables it for comparison.
//
// Totori and Meruru are runtime-confirmed at both 144 and 60 fps. Rorona's
// travel map is different: the stick steps between discrete locations, and
// runtime measurement found its selection cadence unchanged between 144 and
// 60 fps, so this subsystem deliberately installs nothing there.
namespace atfix {

// Totori and Meruru cover both builds and default to on.
bool installWorldMapFix(BYTE* base, const Game& game);

}  // namespace atfix
