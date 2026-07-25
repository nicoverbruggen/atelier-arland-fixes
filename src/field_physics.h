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
// A frame-rate cap was tried first and withdrawn: holding the rate below the
// boundary meant presenting unsynchronized, which tears in exclusive fullscreen
// on Windows. The fix is instead to remove the frame-rate coupling itself, in
// two parts, both ON by default. Each has its own switch, set to 0, so either
// can be A/B'd against the game's own behaviour at runtime.
//
// ARLAND_FIELD_TRACE=1 logs the controller state around each ground-contact
// change, which is how all of this was established. Note it only writes inside
// those windows, so a character resting quietly produces no output at all.
//
// ARLAND_FIELD_ENGINE_FIX rescales the game's own distance constant with frame
// time, so it means a speed rather than a per-frame distance. That is what lets
// the character move at all at high frame rates: vanilla above roughly 700 fps
// cannot walk, because ordinary locomotion no longer clears the per-frame
// distance and every frame's move is reverted. On its own it only reduces the
// resting movement though, because gravity goes on integrating against the
// surface, so a frame still breaks through every few frames.
//
// ARLAND_FIELD_STABILIZER removes that remainder, by holding the character
// while it is genuinely at rest -- grounded, no horizontal velocity, and last
// frame's move reverted -- and in particular by pinning the air timer, so the
// grounded grace period can never expire and no breakthrough frame is needed to
// keep contact. It depends on the rescale and refuses to install without it,
// because the grounded precondition it holds on can otherwise drop while the
// character is still settling.
//
// Both are needed. Measured at a vsync-paced 144 fps on 2026-07-25: with the
// rescale alone the cauldron interaction prompt still flickered, which is the
// residual sawtooth being large enough for a range check to cross; with the
// stabilizer as well it was steady, and standing still produced no ground-
// contact changes at all.
bool installFieldPhysics(BYTE* base, const Game& game);

}  // namespace atfix
