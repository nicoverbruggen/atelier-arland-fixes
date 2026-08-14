// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace arland {

bool initializeGameHooks();
bool frameAtlasCacheEnabled();
bool battleShadowRestoreActive();
void traceMenuPresent(uint64_t durationMicros, uint64_t intervalMicros);

}
