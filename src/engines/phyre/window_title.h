// SPDX-License-Identifier: MIT
#pragma once

// Window-title fix for the multilingual Arland executables.
//
// The Japanese/Chinese build passes a UTF-8-encoded title to the ANSI window API
// (CreateWindowExA). On a system whose ANSI codepage is not UTF-8 (1252 under
// Wine/Proton, and legacy Western/Japanese Windows) that title bar is garbage.
//
// We cannot make it show the real Japanese: the game's window is ANSI-classed
// (IsWindowUnicode == 0), so every title, even one set via SetWindowTextW, is
// marshalled through the system ANSI codepage at the window-proc boundary. On a
// non-UTF-8 codepage the Japanese characters have no mapping and collapse to '?'
// (confirmed by reading the title back after SetWindowTextW: it stored as 003F).
// That happens on Windows too, not just Wine, so decoding the title is a dead end.
//
// What DOES survive that round-trip is plain ASCII. So on a non-UTF-8 codepage we
// replace the unshowable title with a readable ASCII transliteration of it. When
// the ANSI codepage IS UTF-8 the real title already displays, so we stand down.
//
// Gating: only the multilingual build, only when the game language is Japanese,
// and only on a non-Japanese, non-UTF-8 ANSI codepage (where the Japanese title
// cannot render). The substitute is chosen per game from the detected title.
namespace atfix {

// Install the window-title substitution. Best-effort and idempotent: it hooks
// the user32 title APIs only for a recognized multilingual Arland executable
// running in Japanese on a system ANSI codepage that is neither 932 nor UTF-8,
// and is a no-op otherwise. Safe to call before the game creates its window, and
// safe to call more than once.
void installWindowTitleFix();

}  // namespace atfix
