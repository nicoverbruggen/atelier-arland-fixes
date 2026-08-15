// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../../core/hook_util.h"

// Totori asynchronous vertex/index stream lifetime correction.
//
// The game's render producer serializes raw ktgl::CDX11VertexStream and
// ktgl::CDX11IndexStream pointers into commands 0x28 and 0x29. The worker later
// dereferences those wrappers, but the queue owns no reference. Transient effect
// geometry can therefore release a wrapper before its command is consumed.
//
// Both wrapper types use a plain, non-atomic refcount at +8. Keep every refcount
// operation on the producer thread: increment before publishing a command, run
// the original worker handler unchanged, then defer the matching decrement until
// that producer thread next changes a stream binding. This also preserves the
// vertex handler's write to CDX11VertexStream+0x20. The unchanged-draw fast paths
// perform only the same ordinary object loads and comparisons as vanilla.
namespace atfix {

// Totori only, both executable builds. Commands 0x28 and 0x29 serialize raw
// game-wrapper pointers without retaining them. Pin each queued wrapper on its
// producer thread until the render worker has consumed the command.
bool installStreamLifetimeFix(BYTE* base, const Game& game);

}  // namespace atfix
