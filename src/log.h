// Derived from Philip Rebohle's atelier-sync-fix; see LICENSE (zlib).
#pragma once

#include <chrono>
#include <fstream>
#include <iomanip>

#include "util.h"

namespace atfix {

class Log {

public:

  Log(const char* filename)
  : m_file((rotate(filename), filename), std::ios::out | std::ios::trunc) {

  }

  template<typename... Args>
  void operator () (const Args&... args) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(m_mutex);
    if (m_start == std::chrono::steady_clock::time_point{})
      m_start = now;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - m_start).count();
    m_file << '[' << std::setw(8) << ms << "] ";
    (m_file << ... << args) << std::endl;
  }

private:

  mutex         m_mutex;
  std::ofstream m_file;
  std::chrono::steady_clock::time_point m_start;

  // Keep the previous session's log (crash post-mortems included) as
  // <filename>.old instead of truncating it away on launch.
  static void rotate(const char* filename) {
    std::string previous = std::string(filename) + ".old";
    MoveFileExA(filename, previous.c_str(), MOVEFILE_REPLACE_EXISTING);
  }

};

}
