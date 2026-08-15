// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../../core/hook_util.h"

// Totori ShopGoodsList bounds correction.
//
// ShopGoodsList keeps its row records in a vector at +0x60 and the previously
// selected row index at +0x54. The index starts at -1. When input state +0x50
// is 2, the update routine saves a value into the previous row before calling
// the refresh routine that replaces +0x54 with the current selection. If state
// 2 arrives on the first update, the write uses the -1 sentinel:
//
//   begin + (-1 * 0x54) + 0x4c == begin - 8
//
// That overwrites the vector allocation's heap header. Depending on allocator
// layout, the damage is detected immediately during shop teardown or later in
// an unrelated allocation. The fix hides state 2 from the original call only
// when no valid previous row exists, and puts the state back before returning so
// the shop commits on the next update. All valid-row behaviour is unchanged.
namespace atfix {

// Totori only, both builds. Rorona and Meruru do not use this implementation.
bool installShopFix(BYTE* base, const Game& game);

}  // namespace atfix
