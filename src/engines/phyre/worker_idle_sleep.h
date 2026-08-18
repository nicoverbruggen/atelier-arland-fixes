// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>

#include "../../core/hook_util.h"   // Game

// Shortens the idle poll of the worker thread a scene transition waits for.
//
// THE DEFECT. Changing game mode runs to completion inside one call on the game
// thread. Part of that work is handed to a short-lived worker, and the game
// thread then joins it with `WaitForSingleObject(INFINITE)`. The worker's loop
// takes a lock, finds nothing to do, releases the lock and calls `Sleep(500)`
// before rechecking the stop flag its shutdown sets. So asking that worker to
// stop costs up to a full half second, and the game thread spends that half
// second parked, computing nothing.
//
// MEASURED ON RORONA. A battle transition cost 546 to 570 ms, of which one wait
// accounted for 456 to 459 ms across seven sessions, reproducible to within
// 0.6% because it is a constant rather than an amount of work. The whole
// process burned 20 to 60 ms of CPU across it, so nothing was computing. The
// sleeping thread's id matched the joined thread's id exactly, and the measured
// sleep matched the `0x1f4` immediate. Shortening it to 10 ms took the
// transition to 165 to 173 ms with every cache counter unchanged.
//
// THE CORRECTION replaces the argument of that one `Sleep`, selected by its
// caller's return address, and touches nothing else. The lock is released
// before the call, so a shorter sleep only makes the worker recheck a flag more
// often; it does not widen any critical section and it does not change what the
// worker does when it has work. That second path has its own sleep, computed
// from the object's own period and measured at 33 ms, and is deliberately left
// alone: it paces real work, where this one paces nothing.
//
// WHAT ELSE IT REACHES, stated plainly because it is wider than one worker.
// Several threads share this loop body -- two or three are alive at once, and
// the wrapper takes a function pointer, so the same loop serves different jobs.
// All of their idle polls shorten. What each of those jobs is has not been
// established; what is established is that the path being shortened is the one
// where the worker looked, found nothing, and let go of its lock.
//
// 10 ms is where the measurements settled. Lower keeps paying, at about a
// microsecond per microsecond, against a proportional rise in how often each
// worker reacquires its lock. `ARLAND_WORKER_IDLE_SLEEP=0` restores the game's
// own 500 ms for a comparison run; 1 to 500 sets it instead of the default.
//
// ALL SIX BUILDS carry a row. Searching each executable for the six-byte
// `mov ecx, 0x1f4 / call` sequence returns exactly one hit, and the fourteen
// bytes in front of it are identical in every build (`ReleaseMutex` then the
// sleep), so there is nothing to choose between and the six bytes are the whole
// gate. The same pattern exists in Ayesha, which the Dusk project owns, and in
// neither KTGL game.
namespace atfix {

// Installs the override. Declines, with a logged reason, when the feature is
// off, the title is unrecognized, or the build does not carry the expected
// bytes at its row's address.
bool installWorkerIdleSleep(BYTE* base, const Game& game);

// Optional observer for the `Sleep` detour this file owns.
//
// MinHook allows one detour per function, and the blocking probe in
// menu_fix.cpp wants to time sleeps as well. Rather than fight over the hook,
// the probe registers here and is called after each forwarded call, on the
// calling thread, with the value actually passed. Null unregisters.
using SleepObserver = void (*)(DWORD passed, uint64_t nanos, uintptr_t caller);
void setSleepObserver(SleepObserver observer);

}  // namespace atfix
