// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hook_util.h"

namespace atfix {

// Skip the publisher and developer logos shown while the game boots. Installs
// only where the capability matrix supports the feature and the user opted in.
// Returns true when both hooks are live.
bool installLogoSkip(BYTE* base, const Game& game);

}  // namespace atfix
