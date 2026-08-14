// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../../core/hook_util.h"

namespace atfix {

// Skip the opening movie played after the startup logos. Installs only where
// the capability matrix supports the feature and the user opted in. Returns
// true when the hook is live.
bool installMovieSkip(BYTE* base, const Game& game);

}  // namespace atfix
