// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../../core/hook_util.h"

// Startup logo skip.
//
// The boot logos are not part of the title-screen state machine. They belong to
// ThreadEasyRenderLogo, a small object the application creates before it starts
// initialising the engine. Its update runs on the render thread and steps a six
// phase sequence over three fullscreen picture layers (warning text, Koei
// Tecmo, Gust; the English builds start at the second and never show the
// warning). Each logo costs a half-second fade in, two seconds of hold and a
// half-second fade out, so an English boot spends about six seconds here.
//
// The application does not wait for the logos before loading. It creates the
// object, performs the whole engine and resource initialisation while the
// render thread animates, and only then spins until the sequence reports its
// terminal phase. A separate title-side player blocks on the same object for
// the attract replay after an idle title screen. Both wait on nothing but the
// phase field, so writing the terminal phase releases both.
//
// Two hooks, because one is not enough to guarantee a clean screen. Forcing the
// phase stops the sequence advancing, but the picture layers are already
// constructed and their alpha only reaches the material when the layer's own
// update runs, which the forced path no longer calls. Rather than reason about
// what a never-ticked layer draws, the draw is suppressed as well. The result
// is the untouched clear colour for as long as loading genuinely takes, which
// is the honest presentation: skipping the logos does not make loading faster.
//
// The object is left structurally intact, so the game's own destructor still
// frees the picture layers.
namespace atfix {

// Skip the publisher and developer logos shown while the game boots. Installs
// only where the capability matrix supports the feature and the user opted in.
// Returns true when both hooks are live.
bool installLogoSkip(BYTE* base, const Game& game);

}  // namespace atfix
