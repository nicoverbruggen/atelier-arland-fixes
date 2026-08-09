// SPDX-License-Identifier: MIT
#pragma once

namespace atfix {

// Installs a last-chance unhandled-exception filter that writes a post-mortem
// (exception, registers, module+RVA stack scan) to arland-fix.log. Idempotent;
// ARLAND_CRASH_LOG=0 disables it. Installing the process-wide callback pins
// this DLL until process exit so the operating system can never call unmapped
// code; the operation is therefore intentionally irreversible.
void installCrashLogger();

}  // namespace atfix
