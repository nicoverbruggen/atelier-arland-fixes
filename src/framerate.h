// SPDX-License-Identifier: MIT
#pragma once

// Cutscene framerate restore. Some cutscenes (Meruru's especially) raise the
// DXGI present sync interval to 2 — 30 fps on a 60 Hz panel — and an
// interrupt/skip exit misses the restore, leaving the game stuck at 30. We
// already own IDXGISwapChain::Present, so we clamp the interval back to 1 every
// frame: self-healing, so a missed engine restore can never persist. Definitions
// in framerate.cpp.
namespace atfix {

// Whether the clamp is active for the current game (see the CutsceneFramerate
// matrix cell; env ARLAND_CUTSCENE_FRAMERATE / ini [Rendering] CutsceneFramerateFix).
bool cutscenePresentClampEnabled();

// Interval to actually pass to the real Present. Logs each raw-interval change
// (the diagnostic that confirms the 1->2 flip) and clamps anything above 1 to 1.
unsigned int clampPresentInterval(unsigned int syncInterval);

}  // namespace atfix
