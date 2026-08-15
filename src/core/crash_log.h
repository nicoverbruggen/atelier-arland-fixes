// SPDX-License-Identifier: MIT
#pragma once

// Crash logger: a last-chance unhandled-exception filter that appends a
// post-mortem to arland-fix.log before the process dies -- exception code,
// faulting access, registers, and every stack value that looks like a return
// address, each resolved to module+RVA so a crash inside the game or this mod
// can be mapped straight back to a function. Best-effort by design: it only
// reads memory it has VirtualQuery-verified, guards against re-entry, and
// chains to the previously installed filter (Wine/winedbg backtraces and
// PROTON_LOG output are unaffected because the exception continues its search).
namespace atfix {

// Idempotent;
// ARLAND_CRASH_LOG=0 disables it. Installing the process-wide callback pins
// this DLL until process exit so the operating system can never call unmapped
// code; the operation is therefore intentionally irreversible.
void installCrashLogger();

}  // namespace atfix
