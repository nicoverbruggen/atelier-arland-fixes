// SPDX-License-Identifier: MIT
#pragma once

namespace atfix {

// Keep this DLL mapped until process exit. Windows offers no atomic way to
// remove an unhandled-exception filter only if it is still ours, and worker
// callbacks cannot safely run from an image that FreeLibrary has unmapped.
// Call this outside DllMain before publishing either kind of entry point.
// Pinning is intentionally irreversible for the lifetime of the process.
bool retainModuleForProcessLifetime();

}  // namespace atfix
