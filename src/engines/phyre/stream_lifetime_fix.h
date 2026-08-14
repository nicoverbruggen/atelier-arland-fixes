// SPDX-License-Identifier: MIT
//
// Lifetime correction for Totori's asynchronous vertex/index stream commands.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../../core/hook_util.h"

namespace atfix {

// Totori only, both executable builds. Commands 0x28 and 0x29 serialize raw
// game-wrapper pointers without retaining them. Pin each queued wrapper on its
// producer thread until the render worker has consumed the command.
bool installStreamLifetimeFix(BYTE* base, const Game& game);

}  // namespace atfix
