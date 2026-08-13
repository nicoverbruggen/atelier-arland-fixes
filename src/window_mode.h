// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>

namespace atfix {

// Borderless windowed mode: [Rendering] Borderless.
//
// The games offer only windowed (with a title bar and border) or exclusive
// fullscreen. Exclusive fullscreen takes control of the display, which makes
// alt-tabbing slow and, under Wine or Proton, interacts badly with compositors
// and multi-monitor setups. Borderless runs the game as a plain window with its
// decorations removed, sized to fill the monitor it is on: it looks like
// fullscreen, alt-tabs instantly, and leaves the display mode alone.
//
// Off by default, and the default is the game's own exclusive fullscreen, which
// flips straight to the display without asking the compositor for anything.
// Borderless is the better window and it is still a window, so it is offered
// rather than assumed. Borderless=false leaves the game's own fullscreen setting
// to be used as-is.

// Force the swap chain to be created windowed and remember its window. Called
// with the description before the swap chain is created, on both creation
// paths. No-op unless borderless is enabled.
void prepareBorderlessSwapChain(DXGI_SWAP_CHAIN_DESC* desc);

// Strip the window's decorations, size it to its monitor, and take exclusive
// fullscreen out of play: DXGI's alt-enter handling is disabled and the chain is
// pulled back to windowed if anything has already switched it. Call with the
// freshly created swap chain.
void applyBorderlessWindow(IDXGISwapChain* swapChain);

// Re-apply if the game has since restored its own style or size. Cheap enough
// to call every frame; it compares before touching anything.
void maintainBorderlessWindow();

}  // namespace atfix
