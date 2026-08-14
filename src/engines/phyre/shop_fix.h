// SPDX-License-Identifier: MIT
//
// Bounds correction for Totori's ShopGoodsList previous-row write. See
// shop_fix.cpp.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../../core/hook_util.h"

namespace atfix {

// Totori only, both builds. Rorona and Meruru do not use this implementation.
bool installShopFix(BYTE* base, const Game& game);

}  // namespace atfix
