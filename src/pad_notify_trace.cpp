// SPDX-License-Identifier: MIT
//
// See pad_notify_trace.h for the question this answers.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbt.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "pad_notify_trace.h"
#include "log.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// The HID device interface class. Written out rather than taken from
// GUID_DEVINTERFACE_HID in <hidclass.h>, which declares the name but defines it
// only in hid.lib: naming it there would put a link dependency on the whole DLL
// for one constant used by a diagnostic that is off by default.
constexpr GUID kHidInterfaceClass = {
  0x4d1e55b2, 0xf16f, 0x11cf,
  { 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

constexpr wchar_t kWindowClass[] = L"AtelierArlandFixPadNotify";

// How long stopPadNotifyTrace waits for the pump. Bounded because the caller is
// DllMain and a wait that never ends there stops the process from finishing its
// unload.
constexpr DWORD kStopWaitMillis = 2000;

// One registration and the window that receives it. The window is published for
// stopPadNotifyTrace; the notification handle belongs to the pump thread alone.
struct Notification {
  const char* label;
  const GUID* classGuid;      // null asks for every interface class instead
  DWORD extraFlags;
  std::atomic<HWND> window;
  HDEVNOTIFY handle;
};

Notification g_notifications[2] = {
  { "hid", &kHidInterfaceClass, 0, {}, nullptr },
  { "all", nullptr, DEVICE_NOTIFY_ALL_INTERFACE_CLASSES, {}, nullptr },
};

std::atomic<bool> g_started{false};
std::atomic<bool> g_stopRequested{false};
std::atomic<uint64_t> g_events{0};
HANDLE g_thread = nullptr;
// Set by the pump as the last thing it does, and what stopPadNotifyTrace waits
// on. Waiting on the thread handle instead would deadlock: the thread's own exit
// path takes the loader lock that DllMain is holding while it waits.
HANDLE g_finished = nullptr;

// 0 off, 1 message-only windows, 2 hidden top-level windows. See the header for
// what the second mode is for.
int traceMode() {
  const char* value = std::getenv("ARLAND_PAD_NOTIFY_TRACE");
  if (!value || value[0] == '\0' || value[0] == '0')
    return 0;
  return value[0] == '2' ? 2 : 1;
}

// This DLL, for the window class and the windows. Resolved from an address
// inside it because the module handle is not otherwise available here, and
// without a reference count so this cannot itself keep the DLL loaded.
HMODULE selfModule();

LRESULT CALLBACK padNotifyWndProc(HWND, UINT, WPARAM, LPARAM);

HMODULE selfModule() {
  static HMODULE module = [] {
    HMODULE handle = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&padNotifyWndProc), &handle);
    return handle;
  }();
  return module;
}

const char* eventName(WPARAM code) {
  switch (code) {
    case DBT_DEVICEARRIVAL:           return "DBT_DEVICEARRIVAL";
    case DBT_DEVICEREMOVECOMPLETE:    return "DBT_DEVICEREMOVECOMPLETE";
    case DBT_DEVNODES_CHANGED:        return "DBT_DEVNODES_CHANGED";
    case DBT_DEVICEQUERYREMOVE:       return "DBT_DEVICEQUERYREMOVE";
    case DBT_DEVICEQUERYREMOVEFAILED: return "DBT_DEVICEQUERYREMOVEFAILED";
    case DBT_DEVICEREMOVEPENDING:     return "DBT_DEVICEREMOVEPENDING";
    case DBT_DEVICETYPESPECIFIC:      return "DBT_DEVICETYPESPECIFIC";
    case DBT_CUSTOMEVENT:             return "DBT_CUSTOMEVENT";
    default:                          return "unnamed";
  }
}

const char* deviceTypeName(DWORD type) {
  switch (type) {
    case DBT_DEVTYP_OEM:             return "DBT_DEVTYP_OEM";
    case DBT_DEVTYP_DEVNODE:         return "DBT_DEVTYP_DEVNODE";
    case DBT_DEVTYP_VOLUME:          return "DBT_DEVTYP_VOLUME";
    case DBT_DEVTYP_PORT:            return "DBT_DEVTYP_PORT";
    case DBT_DEVTYP_NET:             return "DBT_DEVTYP_NET";
    case DBT_DEVTYP_DEVICEINTERFACE: return "DBT_DEVTYP_DEVICEINTERFACE";
    case DBT_DEVTYP_HANDLE:          return "DBT_DEVTYP_HANDLE";
    default:                         return "unnamed";
  }
}

void formatGuid(const GUID& guid, char (&out)[40]) {
  std::snprintf(out, sizeof(out),
    "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
    static_cast<unsigned>(guid.Data1), static_cast<unsigned>(guid.Data2),
    static_cast<unsigned>(guid.Data3),
    guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
    guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
}

// dbcc_name is a variable-length string running to the end of the structure, so
// how much of it may be read comes from dbch_size and not from the one-element
// array the header declares. The sender is another process, so a size that does
// not match the string, or a string with no terminator, is a case this has to
// survive rather than one it may assume away.
void formatInterfaceName(const DEV_BROADCAST_DEVICEINTERFACE_W* device,
                         char* out, size_t outSize) {
  out[0] = '\0';
  constexpr size_t header = offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name);
  if (device->dbcc_size <= header)
    return;
  const size_t available = (device->dbcc_size - header) / sizeof(WCHAR);
  size_t length = 0;
  while (length < available && device->dbcc_name[length])
    ++length;
  if (!length)
    return;
  const int written = WideCharToMultiByte(CP_UTF8, 0, device->dbcc_name,
    static_cast<int>(length), out, static_cast<int>(outSize) - 1,
    nullptr, nullptr);
  out[written > 0 ? static_cast<size_t>(written) : 0] = '\0';
}

void logDeviceChange(const char* label, WPARAM code, LPARAM data) {
  g_events.fetch_add(1, std::memory_order_relaxed);
  const auto* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(data);
  // DBT_DEVNODES_CHANGED carries no structure at all, so an event with no data
  // is normal and says only that the device tree moved.
  if (!header || header->dbch_size < sizeof(DEV_BROADCAST_HDR)) {
    log("PADNOTIFY reg=", label, " event=", eventName(code), " code=0x",
        std::hex, static_cast<unsigned>(code), std::dec, " data=none");
    return;
  }
  if (header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
    const auto* device =
      reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(header);
    char guid[40] = {};
    formatGuid(device->dbcc_classguid, guid);
    char name[512] = {};
    formatInterfaceName(device, name, sizeof(name));
    log("PADNOTIFY reg=", label, " event=", eventName(code), " code=0x",
        std::hex, static_cast<unsigned>(code), std::dec,
        " type=", deviceTypeName(header->dbch_devicetype),
        "(", header->dbch_devicetype, ")",
        " class=", IsEqualGUID(device->dbcc_classguid, kHidInterfaceClass)
          ? "hid" : "other",
        " guid=", guid,
        " name=", name[0] ? name : "<empty>");
    return;
  }
  log("PADNOTIFY reg=", label, " event=", eventName(code), " code=0x",
      std::hex, static_cast<unsigned>(code), std::dec,
      " type=", deviceTypeName(header->dbch_devicetype),
      "(", header->dbch_devicetype, ")");
}

LRESULT CALLBACK padNotifyWndProc(HWND window, UINT message,
                                  WPARAM wParam, LPARAM lParam) {
  if (message == WM_DEVICECHANGE) {
    const LONG_PTR index = GetWindowLongPtrW(window, GWLP_USERDATA);
    logDeviceChange(
      index >= 0 && index < 2 ? g_notifications[index].label : "unknown",
      wParam, lParam);
    // TRUE is the documented grant for the query events. The others ignore the
    // result, and this window is never in a position to refuse anything.
    return TRUE;
  }
  if (message == WM_DESTROY) {
    // Drop the published handle here rather than in the cleanup below, so the
    // pump never calls DestroyWindow on a handle the window manager has already
    // taken back.
    const LONG_PTR index = GetWindowLongPtrW(window, GWLP_USERDATA);
    if (index >= 0 && index < 2)
      g_notifications[index].window.store(nullptr);
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

HWND createNotifyWindow(int mode, LONG_PTR index) {
  const HWND parent = mode == 2 ? nullptr : HWND_MESSAGE;
  // No WS_VISIBLE in either mode. WS_EX_TOOLWINDOW keeps the top-level variant
  // out of the task switcher and WS_EX_NOACTIVATE keeps it from ever taking
  // focus from the game.
  const DWORD style = mode == 2 ? WS_POPUP : 0;
  const DWORD exStyle = mode == 2 ? (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) : 0;
  HWND window = CreateWindowExW(exStyle, kWindowClass, L"arland-fix pad notify",
    style, 0, 0, 0, 0, parent, nullptr, selfModule(), nullptr);
  if (window)
    SetWindowLongPtrW(window, GWLP_USERDATA, index);
  return window;
}

DWORD WINAPI padNotifyPump(LPVOID) {
  const int mode = traceMode();
  WNDCLASSEXW windowClass = {};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc = &padNotifyWndProc;
  windowClass.hInstance = selfModule();
  windowClass.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&windowClass)) {
    log("PADNOTIFY start=failed stage=class error=", std::dec, GetLastError());
    SetEvent(g_finished);
    return 0;
  }
  log("PADNOTIFY start mode=", mode == 2 ? "top_level" : "message_only");

  bool anyWindow = false;
  for (LONG_PTR index = 0; index < 2; ++index) {
    Notification& notification = g_notifications[index];
    HWND window = createNotifyWindow(mode, index);
    if (!window) {
      log("PADNOTIFY reg=", notification.label, " window=failed error=",
          std::dec, GetLastError());
      continue;
    }
    notification.window.store(window);
    anyWindow = true;

    DEV_BROADCAST_DEVICEINTERFACE_W filter = {};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    if (notification.classGuid)
      filter.dbcc_classguid = *notification.classGuid;
    notification.handle = RegisterDeviceNotificationW(window, &filter,
      DEVICE_NOTIFY_WINDOW_HANDLE | notification.extraFlags);
    // Both halves are reported because a registration that silently failed and
    // a registration that fired for nothing leave the same empty log, and the
    // whole point of the diagnostic is to tell those two apart.
    if (notification.handle)
      log("PADNOTIFY reg=", notification.label, " window=",
          static_cast<void*>(window), " register=ok");
    else
      log("PADNOTIFY reg=", notification.label, " window=",
          static_cast<void*>(window), " register=failed error=",
          std::dec, GetLastError());
  }

  // Dekker's pattern with the store in stopPadNotifyTrace: it sets the flag and
  // then posts to whatever windows are published, this checks the flag after
  // publishing them. Sequential consistency means at least one of the two sees
  // the other, so a stop that arrives while this thread is still starting up
  // cannot be missed.
  if (anyWindow && !g_stopRequested.load()) {
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
      DispatchMessageW(&message);
  }

  for (Notification& notification : g_notifications) {
    HWND window = notification.window.exchange(nullptr);
    if (notification.handle) {
      UnregisterDeviceNotification(notification.handle);
      notification.handle = nullptr;
    }
    if (window)
      DestroyWindow(window);
  }
  UnregisterClassW(kWindowClass, selfModule());
  log("PADNOTIFY stop events=", std::dec, g_events.load());
  SetEvent(g_finished);
  return 0;
}

}  // namespace

void startPadNotifyTrace() {
  if (!traceMode())
    return;
  // One thread and one pair of windows per process, however often this is
  // reached.
  if (g_started.exchange(true))
    return;
  g_finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!g_finished) {
    log("PADNOTIFY start=failed stage=event error=", std::dec, GetLastError());
    return;
  }
  g_thread = CreateThread(nullptr, 0, &padNotifyPump, nullptr, 0, nullptr);
  if (!g_thread) {
    log("PADNOTIFY start=failed stage=thread error=", std::dec, GetLastError());
    CloseHandle(g_finished);
    g_finished = nullptr;
  }
}

void stopPadNotifyTrace() {
  if (!g_started.load() || !g_finished)
    return;
  g_stopRequested.store(true);
  for (Notification& notification : g_notifications) {
    HWND window = notification.window.load();
    if (window)
      PostMessageW(window, WM_CLOSE, 0, 0);
  }
  // A timeout leaves the pump running in code that is about to unmap, so it is
  // worth a line in the log even though there is nothing left to do about it.
  if (WaitForSingleObject(g_finished, kStopWaitMillis) != WAIT_OBJECT_0)
    log("PADNOTIFY stop=timeout");
  CloseHandle(g_finished);
  g_finished = nullptr;
  if (g_thread) {
    CloseHandle(g_thread);
    g_thread = nullptr;
  }
}

}  // namespace atfix
