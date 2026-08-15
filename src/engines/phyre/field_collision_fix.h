// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../../core/hook_util.h"

// Two independent corrections to field character collision, with one shared
// source of confusion between them.
//
// ---------------------------------------------------------------------------
// 1. Monsters snap across the ground when their AI re-targets
// ---------------------------------------------------------------------------
//
// A field monster chasing the player runs a three-step cycle in its brain: mode
// 0 re-targets and builds a mover aimed at the player, mode 2 copies the
// mover's position into the brain's intended position every frame, and mode 1
// notices the mover has expired and starts the next segment. The mover's
// lifetime is set in its constructor as distance divided by speed, so with the
// monster held at a fixed separation from a stationary player the cycle is
// metronomic. Measured at about 540 ms.
//
// The intended position is turned into movement by FieldMapCharaBase::Update,
// which computes velocity = (intended - current) / dt, and the character
// controller then integrates position += velocity * dt. The frame delta
// cancels, so the whole gap is closed in a single frame however long that frame
// is. At the segment boundary the intended position moves abruptly twice, once
// when the mover snaps to its stale target and once when the next segment
// re-anchors, and the character is carried the entire distance in two frames.
//
// The displacement is therefore the same at any frame rate, which is what makes
// this easy to misread: it measures identical at 30, 60 and 200 fps. What
// changes with frame rate is how long those two frames last. The same 0.2 to
// 0.6 unit correction takes about 66 ms at 30 fps and about 10 ms at 200. At
// console frame rate it reads as a brisk step; at high refresh it reads as a
// teleport, and a monster that leaves the staff's reach between two frames
// takes the encounter with it.
//
// The correction spreads the movement over time rather than reducing it. The
// monster still reaches exactly the position the game asked for, and still on
// the same cycle; it simply cannot cover more ground per second than a walking
// character plausibly would. The limit is expressed as a speed rather than as a
// distance per call, because a fixed per-call step would itself be frame-rate
// dependent and would reintroduce the problem in a different form.
//
// Only charas the field map lists as enemies are limited. The player, the party
// and every other character are untouched.
//
// ---------------------------------------------------------------------------
// 2. Characters are pulled together when they are too far apart
// ---------------------------------------------------------------------------
//
// The engine's character-versus-character separation routine computes the
// horizontal distance between two controllers and derives a push depth as the
// sum of their radii minus that distance. The result is signed and nothing
// clamps it, so when the pair is reported with the two further apart than their
// combined radius the depth is negative and the push reverses: the two are
// drawn together until the distance equals the radii sum exactly. That is an
// equality constraint where a separation constraint belongs, and it parks a
// monster on a fixed ring around the player rather than merely keeping it from
// overlapping. It was measured pulling a monster inward from 0.867 to 0.800.
//
// The fix is max(depth, 0), applied as a small code patch because the clamp
// does not fit in the available bytes without displacing instructions that
// cannot be put back. Genuine overlap still separates exactly as before.
//
// This one is not what produced the snapping above, and fixing it alone leaves
// that symptom unchanged. It is corrected because it is wrong, not because it
// was the cause.
//
// Unlike the rate limit, this patch is not scoped to enemies: it changes the
// shared routine, so it applies to every character pair including the player
// and party members. That is the wider of the two changes and has its own
// configuration key so it can be turned off independently.
//
// ---------------------------------------------------------------------------
//
// Both cover all six Arland executables and both ship on by default in all
// three games; the capability matrix in game.cpp is the source of truth.
//
// The separation clamp is PhyreEngine's own defect and is present in every
// Arland build, and in Ayesha. The same instruction sequence with the same
// register allocation occurs exactly once per executable, so one signature
// covers all six and only the address differs.
namespace atfix {

// All six executables. A build whose signatures do not match reports failure
// and installs nothing.
bool installFieldCollisionFix(BYTE* base, const Game& game);

}  // namespace atfix
