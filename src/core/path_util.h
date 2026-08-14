#pragma once

#include <cstddef>
#include <cstring>

namespace atfix {

// Replace the file name at the end of a path with another one, keeping the
// directory. Returns false and leaves the buffer untouched when the path holds
// no separator, or when the new name does not fit.
//
// The fit check is the point of having this in one place. The name going in is
// usually longer than the one coming out, and the length check that produced
// the path only rejects truncation, so a deep install directory leaves a path
// that fits the buffer and a result that does not. Every caller here swaps a
// module file name for a companion file beside it.
inline bool replaceFileName(char* path, size_t size, const char* name) {
  char* back = std::strrchr(path, '\\');
  char* forward = std::strrchr(path, '/');
  char* separator = !back || (forward && forward > back) ? forward : back;
  if (!separator)
    return false;
  const size_t used = size_t(separator + 1 - path);
  const size_t needed = std::strlen(name) + 1;
  if (used + needed > size)
    return false;
  std::memcpy(separator + 1, name, needed);
  return true;
}

}
