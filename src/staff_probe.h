// SPDX-License-Identifier: MIT
//
// Diagnostic probe for Totori's field staff swing. Off unless
// ARLAND_STAFF_PROBE=1. Reports why a swing did or did not start a battle. See
// staff_probe.cpp.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hook_util.h"

namespace atfix {

// Totori English only, and only when ARLAND_STAFF_PROBE=1. Installs nothing in
// any other game or build. Diagnostic: it reads and logs, and changes no
// engine behaviour.
bool installStaffProbe(BYTE* base, const Game& game);

}  // namespace atfix
