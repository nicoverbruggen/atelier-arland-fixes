// SPDX-License-Identifier: MIT
//
// Bounds check for Totori's equipment item-effect and item-trait scans, which
// otherwise turn a bad index in save data into an unbounded read. See
// item_guard.cpp.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../../core/hook_util.h"

namespace atfix {

// Totori only, both builds, on unless ARLAND_ITEM_GUARD=0. Rorona and Meruru
// have no equivalent scan to guard.
bool installItemGuard(BYTE* base, const Game& game);

}  // namespace atfix
