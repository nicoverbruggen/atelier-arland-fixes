// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Records whether a controller plugged in under Proton produces a device-arrival
// notification inside the game's process. It is log-only: it hooks nothing,
// suppresses nothing, changes nothing the game does, and stays asleep unless
// ARLAND_PAD_NOTIFY_TRACE is set.
//
// THE QUESTION IT EXISTS FOR. pad_rescan.h records what the controller rescan
// costs: while no pad is connected the games call IDirectInput8::EnumDevices
// about three times a second, and under Proton that call takes 3.5 ms on
// average. The correction shipping today is a three-second wall-clock floor on
// the retry, which trades a wait for the cost rather than removing either. The
// design that removes both is to suppress every retry and let exactly one
// through when a device actually appears, and that design is only possible if
// the arrival can be observed from inside this process.
//
// WHAT IS ALREADY SETTLED is that the mechanism exists in Wine.
// dlls/user32/input.c implements RegisterDeviceNotificationW for real, through
// I_ScRegisterDeviceNotification and with explicit DBT_DEVTYP_DEVICEINTERFACE
// handling, and programs/plugplay/main.c sends WM_DEVICECHANGE for
// device-interface events. What is NOT settled, and what only a run can answer,
// is whether a game controller arriving through evdev and winebus reaches a
// window in this process as a GUID_DEVINTERFACE_HID arrival.
//
// SO IT REGISTERS TWICE, ON TWO WINDOWS. One notification filters on
// GUID_DEVINTERFACE_HID, which is the answer the next design needs. The other
// asks for every interface class, because "the HID filter never fired" and "no
// device event of any kind reached this process" produce the same empty log and
// call for opposite conclusions. Each registration owns its own window so every
// logged event names which one delivered it; the message itself carries no way
// to tell them apart.
//
// A MESSAGE-ONLY WINDOW DOES NOT RECEIVE BROADCASTS, which is the one way this
// diagnostic could answer no for the wrong reason. Notifications from
// RegisterDeviceNotificationW are sent to the registered window directly and
// arrive either way, but a WM_DEVICECHANGE sent with BroadcastSystemMessageW
// goes to top-level windows only and never reaches HWND_MESSAGE. Setting
// ARLAND_PAD_NOTIFY_TRACE=2 builds the same two windows as hidden top-level
// ones instead, so a silent log from the default mode can be told apart from a
// process that hears nothing whichever window it owns.
//
// It runs on a thread of its own with a pump of its own, and it does not go near
// the game's window or its WndProc. A diagnostic asking a question about this
// process's message handling must not be able to break it.
namespace atfix {

// Starts the trace if ARLAND_PAD_NOTIFY_TRACE is set, and does nothing at all
// otherwise. Safe to call more than once; only one live thread is kept, and a
// failed or already-exited worker can be retried. Not callable from DllMain: it
// creates a thread, which the loader lock makes unsafe there. Starting the
// worker pins this DLL until process exit because its callbacks must stay mapped.
void startPadNotifyTrace();

// Closes the windows, drops both registrations and waits for the pump to finish.
// A no-op when the trace never started. Also not callable from DllMain: it joins
// the actual thread, whose exit may need the loader lock. A timeout leaves the
// retained worker intact so a later call can retry safely.
// No caller today. Kept because the contract above is a pair: a start that
// retains a thread needs a stop that joins it, and a future caller of the one
// needs the other to already exist.
void stopPadNotifyTrace();

}  // namespace atfix
