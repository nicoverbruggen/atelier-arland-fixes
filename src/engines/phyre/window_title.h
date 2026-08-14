// SPDX-License-Identifier: MIT
#pragma once
//
// Window-title substitution for the multilingual (Japanese and Chinese) Arland
// executables. The game passes a UTF-8 title to the ANSI window API, so on a
// system ANSI codepage that is not UTF-8 the title bar renders as mojibake. The
// window is ANSI-classed, so no route can show the real Japanese; this module
// substitutes a per-game ASCII transliteration instead. The reasoning and the
// rejected Unicode route are in window_title.cpp.

namespace atfix {

// Install the window-title substitution. Best-effort and idempotent: it hooks
// the user32 title APIs only for a recognized multilingual Arland executable
// running in Japanese on a system ANSI codepage that is neither 932 nor UTF-8,
// and is a no-op otherwise. Safe to call before the game creates its window, and
// safe to call more than once.
void installWindowTitleFix();

}  // namespace atfix
