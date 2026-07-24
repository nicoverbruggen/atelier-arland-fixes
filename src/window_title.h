// SPDX-License-Identifier: MIT
#pragma once
//
// Self-contained window-title codepage fix for the multilingual (Japanese and
// Chinese) Arland executables. The game hands a CP932 (Shift-JIS) byte string to
// the ANSI window APIs; on a non-Japanese system codepage Windows decodes those
// bytes with the wrong codepage and the title bar renders as mojibake. This
// module hooks the ANSI title-setting APIs, re-decodes the bytes as CP932, and
// pushes the result through the Unicode window API so the title is correct on any
// locale. The definitions live in window_title.cpp.

namespace atfix {

// Install the CP932 window-title fix. Best-effort and idempotent: it hooks the
// user32 title APIs only for a recognized multilingual Arland executable, and is
// a no-op on the English build or when the OS codepage is already Chinese (so a
// native Chinese title is never re-decoded as CP932). Safe to call early (before
// the game creates its window) and safe to call more than once.
void installWindowTitleFix();

}  // namespace atfix
