// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>

#include "hook_util.h"

// Rate-limits the games' controller rescan while no pad is connected.
//
// THE MECHANISM, which is shared by all six DX ports. Gust's input layer keeps a
// pad pointer in a global; while it is null, the per-frame input update retries
// device acquisition. That retry is already gated -- `cmp eax, 0x3c` limits it
// to once per 60 input-update ticks -- and the games do NOT do the thing this
// defect is usually blamed on: there is no per-frame poll of four XInput slots.
// Each executable imports exactly two XInput entries by ordinal, calls
// XInputGetState from exactly two sites, and the 0..3 loop inside the create
// function is a free-slot search over an internal occupancy bitmask that exits
// at the first clear bit.
//
// WHAT IT ACTUALLY COSTS is the other half of the rescan. Ayesha and the three
// Arland games run the descriptor twice per attempt, forcing the IDirectInput8*
// field to null on the first pass, so each rescan is one XInputGetState plus one
// IDirectInput8::EnumDevices(GAMECTRL, ATTACHEDONLY). Escha & Logy and Shallie
// make a single attempt and always pass the live interface, so their XInput path
// is dead code and enumeration is their whole cost. Under Proton that
// enumeration goes through Wine's HID/SetupDi device tree, which is the
// suspected expense -- and hooking XInputGetState, the remedy this defect
// usually gets, would therefore do nothing at all for two of the three Dusk
// games and remove only the cheap half in the third.
//
// THE CORRECTION is a wall-clock floor on the *pad* rescan: the hook returns
// null without calling through unless enough real time has passed since the last
// FAILED attempt. A successful attempt is never suppressed and clears the
// backoff, so hot-plug still works -- just at a longer period than 60 ticks.
// Hooking one function down (the caller that enumerates devices) would also
// suppress the KTGL games' keyboard and third-device retries, which is why the
// pad wrapper specifically is the target.
//
// MEASURED ON AYESHA IN THE SIBLING DUSK PROJECT, where this fix was written
// first, and confirmed by ear in Rorona here. The thresholds set before the
// measurement -- a periodic spike of 3 ms confirms it, under half a millisecond
// falsifies it -- were cleared by a wide margin, with no controller connected
// and the fix off:
//
//   PADRESCAN calls=211 suppressed=0 total_us=732975 mean_us=3473
//             worst_us=9383 fix=0
//
// 3.47 ms is the MEAN, not a rare spike, with a 9.38 ms worst case, 3.3 times a
// second. On a 200 Hz swap chain a frame is 5 ms, so the average call eats 70%
// of one and the worst blows through nearly two. The maintainer saw the tick in
// the same run.
//
// The rate also settles something previously inferred: 3.3 calls per second at
// 200 Hz is one per 60 frames, so the input update does run once per rendered
// frame and the engine's own `cmp eax, 0x3c` gate is counting frames.
//
// It has no ini key. A correction is not a setting -- the same rule that gives
// the travel-map cursor an environment switch and nothing else.
// `ARLAND_PAD_RESCAN=0` stands it down for a comparison, and
// `ARLAND_PAD_RESCAN_PROBE=1` still reports.
//
// ALL SIX BUILDS carry a row. Neither a prologue signature nor the cross-build
// homolog matcher can place this function -- it is a short critical-section
// guard, so its prologue is generic and it has too few n-grams to match -- so
// the multilingual addresses were derived through the import chain instead. See
// pad_rescan.cpp.
namespace atfix {

// One executable's row: the critical-section-guarded pad-create wrapper, and the
// prologue window that proves the build has not moved under the RVA. Defined in
// pad_rescan.cpp itself, unlike the per-build packs of the engine-module fixes.
struct PadRescanTarget {
  uintptr_t wrapperRva;
  std::array<BYTE, 16> expected;
};

// Installs the backoff. Returns true only if the hook went in. Declines, with a
// logged reason, when the feature is off, the row is empty, or the prologue does
// not match.
bool installPadRescanBackoff(BYTE* base, const Game& game);

}  // namespace atfix
