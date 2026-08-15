// SPDX-License-Identifier: MIT
#pragma once

// Replaces the window class's grey background brush with black, so the startup
// flash before the first frame is black rather than mid-grey.
//
// All six executables register their window class with GRAY_BRUSH as the class
// background:
//
//     xor  ecx, ecx            ; r13 = 0 throughout the registration
//     lea  ecx, [r13 + 2]      ; 2 = GRAY_BRUSH
//     call GetStockObject
//     mov  qword ptr [rbp - 0x50], rax   ; WNDCLASSEXA::hbrBackground
//
// The window procedure is short and forwards everything it does not special
// case to DefWindowProcA, so WM_ERASEBKGND is answered by filling the client
// area with that brush. Between the window appearing and the first Present
// there is therefore a mid-grey (128,128,128) rectangle, which is the grey
// screen at startup. It lasts as long as device creation and the first frame's
// work, roughly a second, and it is the game's own doing rather than anything
// Wine or Proton adds; Windows shows it too.
//
// Black is what the game fades up from, so the flash disappears into the
// intro instead of announcing itself. Substituting the brush at registration
// is enough: nothing else reads hbrBackground, and the class is registered
// once.
//
// This hooks RegisterClassExA rather than patching the six call sites because
// the substitution needs no addresses and no prologue gating.
namespace atfix {

// Install from DLL_PROCESS_ATTACH: the class is registered before the game's
// entry point runs, and a hook that arrives later has nothing left to change.
void installWindowBackgroundFix();

}  // namespace atfix
