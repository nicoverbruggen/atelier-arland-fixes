// Derived from Philip Rebohle's atelier-sync-fix; see LICENSE (zlib).
#pragma once

#include <chrono>
#include <sstream>
#include <iomanip>
#include <string>

#include "util.h"

namespace atfix {

// Lock-free by construction, following ReShade's logger (source/dll_log.cpp).
// Each thread formats its whole line into a buffer of its own and hands it to
// one WriteFile; nothing is shared but the file handle, and WriteFile on a
// handle opened for append is atomic per call, so lines from different threads
// interleave whole rather than mixing.
//
// The absence of a lock is the point. The crash filter writes its post-mortem
// through this same Log, from whichever thread faulted. Any lock here can be
// held by a different thread at that moment, and the report then waits on a
// thread that is not going to finish, so the process hangs instead of producing
// the one artifact the logger exists for.
//
// FILE_SHARE_READ so the log can be read while the game holds it open, which is
// what lets a session be watched live rather than after the fact.
//
// No write-through, and nothing buffered on this side either. Every line is
// handed straight to the operating system, whose cache survives the process
// dying, so a crash cannot cost us the lines that describe it. Write-through
// would only add protection against the machine going down, at a disk flush per
// line, and a verbose session can run to hundreds of thousands of lines. The
// crash filter calls flush() so the report reaches the platter even then.
class Log {

public:

  Log(const char* filename) {
    rotate(filename);
    m_file = CreateFileA(filename, FILE_APPEND_DATA,
      FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL, nullptr);
  }

  // Force everything written so far onto the disk. For the crash filter: the
  // ordinary path leaves lines in the operating system's cache, which is safe
  // against the process dying but not against the machine going with it.
  void flush() {
    if (m_file != INVALID_HANDLE_VALUE)
      FlushFileBuffers(m_file);
  }

  ~Log() {
    if (m_file != INVALID_HANDLE_VALUE)
      CloseHandle(m_file);
  }

  template<typename... Args>
  void operator () (const Args&... args) {
    if (m_file == INVALID_HANDLE_VALUE)
      return;
    const auto now = std::chrono::steady_clock::now();
    if (m_start.load(std::memory_order_relaxed) ==
        std::chrono::steady_clock::time_point{}) {
      std::chrono::steady_clock::time_point unset{};
      m_start.compare_exchange_strong(unset, now, std::memory_order_relaxed);
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - m_start.load(std::memory_order_relaxed)).count();

    // One stream per thread, reused. Formatting is where the old shared stream
    // needed the lock; giving each thread its own removes the need for one.
    // Cleared rather than reconstructed so a hot log line does not allocate.
    thread_local std::ostringstream line;
    line.str(std::string());
    line.clear();
    // std::dec guards against a previous line's sticky std::hex manipulator
    // bleeding into the timestamp.
    line << std::dec << '[' << std::setw(8) << ms << "] ";
    (line << ... << args) << "\r\n";

    const std::string text = line.str();
    DWORD written = 0;
    WriteFile(m_file, text.data(), static_cast<DWORD>(text.size()),
      &written, nullptr);
  }

private:

  HANDLE m_file = INVALID_HANDLE_VALUE;
  std::atomic<std::chrono::steady_clock::time_point> m_start{};

  // Keep the previous session's log (crash post-mortems included) as
  // <filename>.old instead of truncating it away on launch.
  static void rotate(const char* filename) {
    std::string previous = std::string(filename) + ".old";
    MoveFileExA(filename, previous.c_str(), MOVEFILE_REPLACE_EXISTING);
  }

};

}
