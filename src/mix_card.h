// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hook_util.h"

// Synthesis product-card animation rate.
//
// `Card::Update` is the fixed-timestep pump behind the card animation shown
// while synthesising. Its loop is bottom-tested: the five bytes where the
// pre-test belongs are an alignment NOP, so the body runs at least once per
// rendered frame. It computes `n = (int)(accumulator * 59.94)` and then ticks
// `max(1, n)` times, never zero. At 59.94 Hz and below that is correct, because
// n is already at least 1. Above it, n is always 0 and the pump ticks anyway,
// so the tick rate becomes the frame rate: about 2.4 times too fast at 144 Hz
// and 3.34 times at 200 Hz. A corroborating symptom is that the accumulator
// drifts unboundedly negative above 60 fps, because the subtraction still fires
// on every frame whose count was zero.
//
// The correction supplies the missing pre-test and nothing else. When a tick is
// due the original runs completely untouched, so shipped behaviour is preserved
// bit-for-bit at 59.94 Hz and below. When it is not, the elapsed time is banked
// and the frame skips the tick.
//
// Ported from the Dusk project, where the same defect and the same correction
// are confirmed in game in Escha & Logy and Shallie. The idiom occurs exactly
// once per executable and the prologue is byte-identical in all six Arland
// builds, so one verification window covers every one of them.
namespace atfix {

// Hook Card::Update for the running executable. Returns true when live.
bool installMixCardFix(BYTE* base, const Game& game);

// Called every Present; no-ops unless ARLAND_MIXCARD_PROBE is set, and then
// reports ticked-versus-skipped counts periodically.
void mixCardReport();

}  // namespace atfix
