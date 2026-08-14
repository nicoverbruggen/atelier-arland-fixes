#!/usr/bin/env python3
"""Protect callback, worker and crash-log lifetime invariants."""

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def fail(message):
    print(f"lifecycle contract check failed: {message}", file=sys.stderr)
    return 1


def function_body(text, signature, label):
    start = text.find(signature)
    if start < 0:
        raise ValueError(f"could not find {label}")
    opening = text.find("{", start + len(signature))
    if opening < 0:
        raise ValueError(f"could not find body of {label}")
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[opening + 1:index]
    raise ValueError(f"unterminated body of {label}")


def before(body, first, second, message):
    left = body.find(first)
    right = body.find(second)
    if left < 0 or right < 0 or left >= right:
        raise ValueError(message)


def main():
    try:
        main_cpp = (ROOT / "src" / "core" / "main.cpp").read_text()
        crash = (ROOT / "src" / "core" / "crash_log.cpp").read_text()
        pad = (ROOT / "src" / "core" / "pad_notify_trace.cpp").read_text()
        log_h = (ROOT / "src" / "core" / "log.h").read_text()
        lifetime = (ROOT / "src" / "core" / "module_lifetime.cpp").read_text()
        meson = (ROOT / "meson.build").read_text()

        for flag in (
            "GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS",
            "GET_MODULE_HANDLE_EX_FLAG_PIN",
        ):
            if flag not in lifetime:
                raise ValueError(f"module retention lost {flag}")
        if "retained.store(true, std::memory_order_release)" not in lifetime:
            raise ValueError("module retention is not release-published")
        if "src/core/module_lifetime.cpp" not in meson:
            raise ValueError("module retention implementation is not built")

        install = function_body(
            crash, "void installCrashLogger()", "crash logger installer"
        )
        before(
            install, "retainModuleForProcessLifetime()",
            "SetUnhandledExceptionFilter(&crashFilter)",
            "crash callback is published before the DLL is retained",
        )

        start = function_body(
            pad, "void startPadNotifyTrace()", "pad trace starter"
        )
        before(
            start, "retainModuleForProcessLifetime()", "CreateThread(",
            "pad worker is created before the DLL is retained",
        )
        before(
            start, "HANDLE thread = CreateThread(", "g_thread = thread;",
            "pad worker handle is published before CreateThread succeeds",
        )
        if "g_started" in pad or "g_finished" in pad:
            raise ValueError("pad lifecycle still uses poisonable/signal-only state")

        stop = function_body(
            pad, "void stopPadNotifyTrace()", "pad trace stopper"
        )
        if "WaitForSingleObject(thread, kStopWaitMillis)" not in stop:
            raise ValueError("pad stop no longer joins the actual worker thread")
        dll_main = function_body(main_cpp, "BOOL WINAPI DllMain(", "DllMain")
        if "stopPadNotifyTrace" in dll_main:
            raise ValueError("DllMain waits for the pad worker under loader lock")

        if "WriteFile(" not in log_h or "FlushFileBuffers(" not in log_h:
            raise ValueError("crash logger is not direct-write and flushable")
        if "m_mutex" in log_h or "std::ofstream" in log_h:
            raise ValueError("crash logger can deadlock on a shared stream lock")
        if "log.flush();" not in crash:
            raise ValueError("crash report is not flushed before chaining")

    except (OSError, ValueError) as exc:
        return fail(str(exc))

    print("lifecycle contract ok: retained callbacks, joined worker, crash-safe log")
    return 0


if __name__ == "__main__":
    sys.exit(main())
