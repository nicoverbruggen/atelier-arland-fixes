// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../../core/hook_util.h"

// The waits in front of the save data slots view.
//
// Opening the save data slots view is slow, and the delay is not file access.
// It is a set of hardcoded wall-clock waits, which is why it is just as slow on
// a test rig where the Steam storage calls are local file operations.
//
// Each wait has the same shape: an accumulator gains the frame delta, is
// compared against a constant from a shared read-only float pool, and a
// conditional branch skips the work until the constant is reached.
//
//   movss  xmm0, [obj + acc]
//   comiss xmm0, [rip + constant]
//   jb/jbe skip           <- the branch this replaces with NOPs
//   ... the work the wait holds up ...
//
// There are two ways into the view and both are gated:
//
//   - from the main menu: 0.3 s before the helper object is even allocated,
//     then 0.5 s before the view is opened. 0.8 s in total.
//   - from inside the Atelier: 1.5 s before the view is opened. A separate
//     1.5 s gate on the way back out.
//
// The constants must not be patched. They are shared float-pool entries: the
// 0.3 and 0.5 sit beside a 0.4 that other code reads, and each 1.5 is a single
// pooled dword with many references. The branch is what changes.
//
// Why this is safe to do, established by reading each site:
//
//   - No gate polls I/O, an async load, or object readiness. Every condition is
//     pure elapsed time. The one exception proves the rule: the in-game exit
//     gate polls a fade's busy flag *and then* waits 1.5 s on top, so the timer
//     is pacing layered over the animation rather than standing in for it.
//   - The first gate branches straight to the accumulator update and returns,
//     with nothing in between, and the work it holds up is an allocation and a
//     constructor that build four sub-objects synchronously. Nothing to wait on.
//   - Nothing fires twice. Each gate's fall-through path sets the state that
//     stops it being reached again: the first stores the new object pointer,
//     the second advances a state field, and the in-game gates set a one-shot
//     flag on the same path.
//
// Rorona and Meruru carry a fifth gate that Totori does not: a dispatch gate in
// front of a jump table of scene-open calls, reached on a re-entry path.
//
// A caveat that reading the code raised and playing it withdrew. Rorona's title
// gate sits in a function that also starts a fade of 0.5 s, from the same pool
// entry the gate compares against, which read like the wait pacing a real
// animation one for one. In play there is no fade on Rorona at all, before or
// after, so whatever that object is it does not show. The other two games do
// fade into the view, Totori slightly to white and Meruru to black, and neither
// is affected. The matching constant was a coincidence, or the fade runs on
// something invisible; either way it does not constrain this.
//
// Removing the waits exposed a second defect, which this fix also corrects: see
// "the carried press" in fast_save_menu.cpp.
//
// Measured on Totori after these were removed: the view opens immediately, and
// what remains is a single text render of roughly 9 ms when the highlighted slot
// changes. The per-frame row rebuild everyone expected to be the cost measures
// 36 microseconds a call, under one percent of wall clock, and moving the cursor
// touches no file or storage call at all.
namespace atfix {

// All three games, both builds each.
bool installSaveMenuFix(BYTE* base, const Game& game);

}  // namespace atfix
