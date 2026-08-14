// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hook_util.h"

namespace atfix {

// Field-map character movement: two corrections, both on by default, sharing
// one detour on the controller's per-frame update.
//
// THE GROUND RAY fixes characters bouncing on uneven ground. The engine holds a
// character up by cancelling its vertical velocity on any frame it has ground
// contact, and keeps the grounded flag alive for a fixed 0.0666667 seconds
// after contact is lost -- without stopping gravity for that window. A
// character whose contact flickers therefore free-falls while the engine still
// considers it grounded, and the whole accumulated drop is corrected in one
// frame when contact returns. Fall, fall, fall, jump, about fifteen times a
// second. The amplitude follows from a wall-clock constant, so it is the same
// at any frame rate and only its appearance changes: a fast buzz at high
// refresh, bumpy walking at 60. It is worst on roaming monsters, which are
// never still enough for the engine's own ground snap to run.
//
// The correction casts a short ray down from the feet after the frame's
// movement, and where it finds ground, puts the character on it and takes its
// vertical velocity away. What is left is one gravity step per frame, about a
// hundred times smaller than the bounce it replaces. Two details carry it: the
// ray runs AFTER the update, so the height matches the position the character
// actually ends the frame at rather than the one it left; and the reach is
// short, so a character walking off a ledge misses the ray and falls normally.
//
// ARLAND_FIELD_GROUND_RAY turns it off. ARLAND_FIELD_GRACE_HOLD turns off the
// weaker fallback that covers frames where the ray finds no ground. Both also
// answer to [Debug] FieldJitterFix, which the launcher exposes, so one control
// gives back the engine's own field movement.
//
// THE THRESHOLD RESCALE fixes a different defect that looks unrelated until it
// bites: the collision resolver discards any frame in which the character moves
// less than 0.0085 world units in total, reverting the position. That is a
// per-frame distance, so as a SPEED FLOOR it rises with the frame rate, at
// 0.0085 * fps:
//
//     60 fps   0.51 units/s      the dead zone the game was designed around
//    144 fps   1.22 units/s
//    200 fps   1.70 units/s      about the speed a player actually moves at
//    600 fps   5.10 units/s      above full running speed: no movement at all
//
// Measured player speed is 1.9 to 2.3 units/s walking and 4.4 to 5.4 running,
// so this is not a hypothetical guarding some unreachable frame rate. By 200 fps
// the floor has reached ordinary walking speed and slow movement is being
// discarded; 600 is only where it passes running speed and the character stops
// entirely.
//
// Rescaling the constant with the frame time pins the floor at its 60 fps value
// whatever the refresh. The ground ray does not help here and cannot: the revert
// restores the whole position vector, correction included.
//
// ARLAND_FIELD_ENGINE_FIX turns it off.
//
// A resting stabilizer used to sit alongside these, holding a character that
// was grounded, horizontally still, and whose last move the resolver threw
// away. The ground ray covers that case and more -- it reaches a character that
// is moving, which the stabilizer structurally could not -- so it was removed
// once the two were measured against each other.
bool installFieldPhysics(BYTE* base, const Game& game);

}  // namespace atfix
