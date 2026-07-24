// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hook_util.h"

namespace atfix {

// Field-map character jitter.
//
// Standing on a step or ledge, the character buzzes vertically above roughly
// 115 fps and is steady below it. The cause is a constant that was only ever
// right at 60: the collision resolver discards any frame in which the character
// moves less than 0.0085 world units in total, reverting the position and
// skipping the ground-snap that would re-seat it. Resting on a surface the only
// motion is one frame of gravity, which at 60 fps clears that distance in two
// frames but at 144 fps takes twelve — longer than the 66.7 ms grace period the
// grounded flag is held for. The flag drops, the character falls until velocity
// has built enough for a frame to clear the threshold, lands, and repeats.
//
// The fix users get is the frame-rate cap in main.cpp ([Engine] MaxFps),
// which keeps the rate below where this begins without touching the game.
//
// Nothing here is enabled by default. ARLAND_FIELD_TRACE=1 logs the controller
// state around each ground-contact change, which is how the above was
// established. ARLAND_FIELD_ENGINE_FIX=1 additionally rescales the game's own
// distance constant with frame time, which keeps the full refresh rate but only
// reduces the movement rather than removing it, and writes to the game's memory
// -- kept for investigation, not offered as an option.
bool installFieldPhysics(BYTE* base, const Game& game);

}  // namespace atfix
