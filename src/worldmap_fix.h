// SPDX-License-Identifier: MIT
//
// Frame-rate-independent travel-map analog cursor movement for Totori and
// Meruru. See worldmap_fix.cpp.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hook_util.h"

namespace atfix {

// Totori and Meruru cover both builds and default to on. Rorona's discrete
// selector is not frame-rate coupled and is deliberately left unchanged.
bool installWorldMapFix(BYTE* base, const Game& game);

}  // namespace atfix
