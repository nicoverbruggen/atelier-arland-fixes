// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../../core/hook_util.h"

// Intro movie skip for Rorona, Totori and Meruru.
//
// The game plays its movies through one open routine that takes the player
// object and an index into a small table of movies. The table is four entries
// of 0x20 bytes, each holding a file name, a frame size and a caption; index 0
// is `opening.wmv`, and the rest are the endings.
//
// The skip does not invent a code path. The open routine already begins by
// asking whether the movie subsystem is ready, and when the answer is no it
// writes 1 to the player's state byte and returns without opening anything.
// The per-frame movie update reads that same byte first and returns "not
// playing" immediately, so the caller advances as though the movie had
// finished. That is the engine's own graceful degradation for a movie it
// cannot play, which means the surrounding code is already written to handle
// it. The detour reproduces exactly that: state byte to 1, no original call.
//
// SAVE DATA IS NOT AT RISK. Every call site of the open routine, in all six
// builds, sets the movie's gallery seen-bit with `bts` into a bitset before
// calling in, so detouring the open routine cannot cost the player an unlock.
// (Rorona EN sets it at 0x3c636d and 0x3c63b7 and calls at 0x3c6415; Totori has
// three call sites and Meruru one, all the same shape.)
//
// WHAT IS SKIPPED: the first movie the process plays, and nothing after. See
// consumeStartupMovieBudget in movie_skip.cpp for why the rule counts plays
// rather than reading the index the routine is handed.
namespace atfix {

// Skip the opening movie played after the startup logos. Installs only where
// the capability matrix supports the feature and the user opted in. Returns
// true when the hook is live.
bool installMovieSkip(BYTE* base, const Game& game);

}  // namespace atfix
