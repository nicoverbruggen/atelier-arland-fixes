// SPDX-License-Identifier: MIT
//
// Replaces the window class's grey background brush with black, so the startup
// flash before the first frame is black rather than mid-grey. See
// window_background.cpp.
#pragma once

namespace atfix {

// Install from DLL_PROCESS_ATTACH: the class is registered before the game's
// entry point runs, and a hook that arrives later has nothing left to change.
void installWindowBackgroundFix();

}  // namespace atfix
