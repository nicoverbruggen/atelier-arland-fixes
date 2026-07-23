// SPDX-License-Identifier: MIT
#pragma once

namespace atfix {

// Installs a last-chance unhandled-exception filter that writes a post-mortem
// (exception, registers, module+RVA stack scan) to arland-fix.log. Idempotent;
// ARLAND_CRASH_LOG=0 disables it.
void installCrashLogger();

}  // namespace atfix
