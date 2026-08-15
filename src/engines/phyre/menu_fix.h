// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace arland {

bool initializeGameHooks();

// Whether atlas snapshots may live for the whole menu-construction frame rather
// than for one queue drain. On by default in Rorona and Totori. Meruru is opt-in
// because it measurably buys nothing there: its queue drain already serves all
// but three of the reads, so the longer lifetime would be exposure without a
// win.
bool frameAtlasCacheEnabled();
bool battleShadowRestoreActive();
void traceMenuPresent(uint64_t durationMicros, uint64_t intervalMicros);

}
