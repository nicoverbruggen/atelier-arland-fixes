// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../../core/hook_util.h"   // Game

// Restores Meruru's battle-entry transition on every battle instead of the first.
//
// THE DEFECT. Entering a battle plays a full-screen effect: a snapshot of the
// field mapped onto an animated mesh. It plays on the first battle of a session
// and shows nothing on every later one. The effect is still constructed,
// updated, rendered and reaped, so it is not failing to play; it plays with
// nothing on it. The defect is the game's own and reproduces with this DLL
// removed entirely.
//
// THE CORRECTION IS FOUR DETOURS THAT DO NOTHING. Each calls the original and
// returns. Nobody knows why that works, and this comment is not standing in for
// an explanation kept somewhere else.
//
// The count is not negotiable. Detouring only the capture enqueue does not
// restore the animation; neither does detouring only the render, nor the render
// together with the vertex count at `0x44ba40`. Each was built and run. Fewer
// than four has never worked, so install all four or none.
//
// KEEP THE BODIES EMPTY. A version that recorded two values behaved exactly like
// one that recorded nothing, and anything added here changes a correction nobody
// can reason about.
//
// SCOPE. Meruru English only, and that is measured rather than cautious. Rorona
// plays the same effect through the same function and shows it in every battle,
// with the mod and without. Totori never constructs the effect. The multilingual
// addresses have not been derived, and the byte gate declines rather than
// hooking anything when the executable does not match.
// `ARLAND_BATTLE_TRANSITION_FIX=0` hands the four addresses back.
//
// ONE HOOK PER ADDRESS. MinHook allows one, so nothing else in the mod may
// detour these four, and an instrument placed on one of them can change the
// outcome it is measuring. `HANDOFF-meruru-battle-transition.md` holds the
// investigation: what was excluded, what was not, and what to measure next.
namespace atfix {

// Installs the correction. Declines, with a logged reason, when the switch is
// off, the game is not the mapped Meruru build, or any of the four prologues
// differs.
bool installBattleTransitionFix(BYTE* base, const Game& game);

}  // namespace atfix
