// SPDX-License-Identifier: MIT
//
// High-resolution UI text. The Arland games draw UI text from a pre-baked bitmap
// font blitted 1:1, so on-screen text is low-resolution. For each string this
// substitutes a kScale× bitmap into the engine's per-string output object, keeping
// the normalized used/pot metrics so the on-screen quad is unchanged in size and
// position — only its texel density increases. Two modes, selected by
// [Rendering] Font / ARLAND_UIFONT: "replaced" (the default) re-renders the string
// from a bundled scalable font (Cuprum), falling back to "upscaled" for glyphs it
// can't resolve; "upscaled" filter-upscales the engine's own baked glyphs,
// preserving its exact layout (multi-line/align/icons). Active on any recognized
// Arland title, English builds only. Implementation in font_hires.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace atfix {

using HiResAllocFn = void* (*)(size_t);   // must match the engine's allocator
using HiResFreeFn = void (*)(void*);      // (the engine frees the buffer)

// Whether high-resolution UI text is active ([Rendering] Font = replaced or
// upscaled, on any recognized Arland title; English builds only). Cached.
bool hiResTextEnabled();

// After the game's renderText has produced its output object at renderer+0x1a0,
// build a high-resolution bitmap for `utf8` (re-rendered from the bundled font, or
// the baked bitmap filter-upscaled) and write it back, reallocating the object's
// pixel buffer through `alloc`/`free` (which must be the engine's own allocator)
// and preserving the metrics. Returns true if it substituted; false/no-op on
// disable or any failure, leaving the engine's original low-res bitmap in place.
bool hiResTextRerender(uintptr_t renderer, const char* utf8,
                       HiResAllocFn alloc, HiResFreeFn free);

// Restore the output object's width/height (`+0`/`+4`) to their pre-substitution
// values. The doubled dims are needed while the text consumer builds its texture
// and copies the bitmap; afterward the engine's auto-size widgets recompute the
// on-screen size as `fraction * dimension`, so the dims must read as the original
// (pre-double) values or the text renders scaled up. Call once the consumer that
// wraps renderText has returned (it consumes a thread-local stash set by the most
// recent substitution; a no-op if there was none). Keeps the on-screen size
// identical while the texture stays high-resolution.
void hiResTextRestoreDims();

}  // namespace atfix
