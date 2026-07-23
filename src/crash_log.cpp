// SPDX-License-Identifier: MIT
//
// Crash logger: a last-chance unhandled-exception filter that appends a
// post-mortem to arland-fix.log before the process dies — exception code,
// faulting access, registers, and every stack value that looks like a return
// address, each resolved to module+RVA so a crash inside the game or this mod
// can be mapped straight back to a function. Best-effort by design: it only
// reads memory it has VirtualQuery-verified, guards against re-entry, and
// chains to the previously installed filter (Wine/winedbg backtraces and
// PROTON_LOG output are unaffected because the exception continues its search).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "crash_log.h"
#include "log.h"

namespace atfix {

extern Log log;   // lives in main.cpp

namespace {

LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;
std::atomic<bool> handlingCrash = { false };

bool readableRange(uintptr_t address, size_t size) {
  MEMORY_BASIC_INFORMATION info = { };
  if (!VirtualQuery(reinterpret_cast<const void*>(address), &info,
      sizeof(info)))
    return false;
  if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) ||
      (info.Protect & PAGE_NOACCESS))
    return false;
  const uintptr_t regionEnd =
    reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
  return address + size <= regionEnd;
}

bool executableAddress(uintptr_t address) {
  MEMORY_BASIC_INFORMATION info = { };
  if (!VirtualQuery(reinterpret_cast<const void*>(address), &info,
      sizeof(info)))
    return false;
  if (info.State != MEM_COMMIT)
    return false;
  return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Format an address as "module.dll+0xRVA" when it falls inside a loaded
// module, "0xADDRESS" otherwise. Returns the caller-provided buffer.
const char* describeAddress(uintptr_t address, char* buffer, size_t size) {
  HMODULE module = nullptr;
  char path[MAX_PATH] = { };
  if (GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(address), &module) && module &&
      GetModuleFileNameA(module, path, sizeof(path))) {
    const char* back = std::strrchr(path, '\\');
    const char* forward = std::strrchr(path, '/');
    const char* sep = back > forward ? back : forward;
    const char* name = sep ? sep + 1 : path;
    const uintptr_t rva = address - reinterpret_cast<uintptr_t>(module);
    wsprintfA(buffer, "%s+0x%Ix", name, rva);
  } else {
    wsprintfA(buffer, "0x%Ix", address);
  }
  buffer[size - 1] = '\0';
  return buffer;
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* pointers) {
  if (handlingCrash.exchange(true, std::memory_order_acq_rel))
    return EXCEPTION_CONTINUE_SEARCH;

  const EXCEPTION_RECORD* record =
    pointers ? pointers->ExceptionRecord : nullptr;
  const CONTEXT* context = pointers ? pointers->ContextRecord : nullptr;
  char describe[MAX_PATH + 32] = { };

  if (record) {
    log("CRASH code=", std::hex, record->ExceptionCode,
      " at ", describeAddress(
        reinterpret_cast<uintptr_t>(record->ExceptionAddress),
        describe, sizeof(describe)), std::dec);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        record->NumberParameters >= 2) {
      const ULONG_PTR kind = record->ExceptionInformation[0];
      log("CRASH access ", kind == 0 ? "read" : (kind == 1 ? "write"
        : "execute"), " of 0x", std::hex,
        uintptr_t(record->ExceptionInformation[1]), std::dec);
    }
  }

  if (context) {
    log("CRASH rip=0x", std::hex, uintptr_t(context->Rip),
      " rsp=0x", uintptr_t(context->Rsp),
      " rbp=0x", uintptr_t(context->Rbp), std::dec);
    log("CRASH rax=0x", std::hex, uintptr_t(context->Rax),
      " rbx=0x", uintptr_t(context->Rbx),
      " rcx=0x", uintptr_t(context->Rcx),
      " rdx=0x", uintptr_t(context->Rdx), std::dec);
    log("CRASH rsi=0x", std::hex, uintptr_t(context->Rsi),
      " rdi=0x", uintptr_t(context->Rdi),
      " r8=0x", uintptr_t(context->R8),
      " r9=0x", uintptr_t(context->R9), std::dec);
    log("CRASH r10=0x", std::hex, uintptr_t(context->R10),
      " r11=0x", uintptr_t(context->R11),
      " r12=0x", uintptr_t(context->R12),
      " r13=0x", uintptr_t(context->R13), std::dec);
    log("CRASH r14=0x", std::hex, uintptr_t(context->R14),
      " r15=0x", uintptr_t(context->R15), std::dec);

    // Conservative stack scan: log every stack slot that points into
    // executable module code. Noisier than a real unwind (stale return
    // addresses linger on the stack) but needs no unwind tables and cannot
    // itself fault. Outermost frames come first.
    const uintptr_t stackTop = uintptr_t(context->Rsp);
    uint32_t logged = 0;
    for (uintptr_t slot = stackTop;
         slot < stackTop + 0x2000 && logged < 32; slot += sizeof(uintptr_t)) {
      if (!readableRange(slot, sizeof(uintptr_t)))
        break;
      uintptr_t value = 0;
      std::memcpy(&value, reinterpret_cast<const void*>(slot), sizeof(value));
      if (!value || !executableAddress(value))
        continue;
      log("CRASH stack[+0x", std::hex, slot - stackTop, std::dec, "] ",
        describeAddress(value, describe, sizeof(describe)));
      ++logged;
    }
  }
  log("CRASH end of report");

  handlingCrash.store(false, std::memory_order_release);
  return previousFilter
    ? previousFilter(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void installCrashLogger() {
  static const bool once = [] {
    const char* value = std::getenv("ARLAND_CRASH_LOG");
    if (value && value[0] == '0')
      return false;
    previousFilter = SetUnhandledExceptionFilter(&crashFilter);
    log("Crash logger installed (previous filter=",
      reinterpret_cast<void*>(previousFilter), ")");
    return true;
  }();
  (void)once;
}

}  // namespace atfix
