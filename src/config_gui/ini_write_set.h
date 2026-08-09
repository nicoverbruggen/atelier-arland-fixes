// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstring>

namespace atfix::launcher {

// The final intended state of every key touched by one save. Win32/Wine may
// report a write or cache flush failure after changing some or all keys; only
// an exhaustive readback can decide whether the save actually reached disk.
// Repeated writes to one key replace its expected final state.
class IniWriteSet {
public:
  void clear() {
    entries_ = {};
    count_ = 0;
    complete_ = true;
  }

  bool note(const char* section, const char* key, const char* value) {
    if (!section || !key)
      return true;  // cache flush, not a keyed mutation
    size_t index = count_;
    for (size_t i = 0; i < count_; ++i) {
      if (!lstrcmpiA(entries_[i].section, section) &&
          !lstrcmpiA(entries_[i].key, key)) {
        index = i;
        break;
      }
    }
    if (index == count_) {
      if (count_ == entries_.size()) {
        complete_ = false;
        return false;
      }
      ++count_;
    }
    Entry& entry = entries_[index];
    if (std::strlen(section) >= sizeof(entry.section) ||
        std::strlen(key) >= sizeof(entry.key) ||
        (value && std::strlen(value) >= sizeof(entry.value))) {
      complete_ = false;
      return false;
    }
    lstrcpyA(entry.section, section);
    lstrcpyA(entry.key, key);
    entry.deleted = value == nullptr;
    entry.value[0] = '\0';
    if (value)
      lstrcpyA(entry.value, value);
    return true;
  }

  bool verify(const char* path) const {
    if (!complete_ || !path || !path[0] || count_ == 0)
      return false;
    static constexpr char missing[] = "__ATFIX_KEY_MISSING_9A31__";
    for (size_t i = 0; i < count_; ++i) {
      char readBack[128] = {};
      GetPrivateProfileStringA(entries_[i].section, entries_[i].key, missing,
        readBack, sizeof(readBack), path);
      if (entries_[i].deleted) {
        if (lstrcmpA(readBack, missing) != 0)
          return false;
      } else if (!lstrcmpA(readBack, missing) ||
                 lstrcmpA(readBack, entries_[i].value) != 0) {
        return false;
      }
    }
    return true;
  }

  size_t size() const { return count_; }

private:
  struct Entry {
    char section[32] = {};
    char key[32] = {};
    char value[96] = {};
    bool deleted = false;
  };

  std::array<Entry, 48> entries_ = {};
  size_t count_ = 0;
  bool complete_ = true;
};

}  // namespace atfix::launcher
