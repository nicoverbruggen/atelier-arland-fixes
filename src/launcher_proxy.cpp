// 32-bit MSIMG32 proxy for the two Arland front-ends. Original project code
// under the MIT terms in ../LICENSE; it modifies only the verified process
// image in memory.
//
// Both ArlandDXLauncher.exe (what Steam runs) and ArlandDXEnv.exe (the settings
// editor) import msimg32, so this one DLL is loaded into each and does a
// different job in each:
//
//   ArlandDXLauncher.exe -> start arland-fix-launcher.exe instead, if it is
//                           installed, or the game itself when arland-fix.ini
//                           asks for that with [Launcher] SkipLauncher
//   ArlandDXEnv.exe      -> add 1440p and 4K to the resolution list
//
// Everything else that loads it just gets the two forwarded GDI entry points.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace {

void launcherLog(const char* message) {
#ifdef ARLAND_LAUNCHER_DIAGNOSTIC
  std::array<char, 32768> path = { };
  const DWORD length = GetModuleFileNameA(nullptr, path.data(), path.size());
  if (!length || length == path.size())
    return;
  char* name = path.data();
  for (char* cursor = path.data(); *cursor; cursor++) {
    if (*cursor == '\\' || *cursor == '/')
      name = cursor + 1;
  }
  std::memcpy(name, "arland-launcher.log", sizeof("arland-launcher.log"));
  HANDLE file = CreateFileA(path.data(), FILE_APPEND_DATA,
    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;
  DWORD written = 0;
  WriteFile(file, message, static_cast<DWORD>(std::strlen(message)),
    &written, nullptr);
  static constexpr char newline[] = "\r\n";
  WriteFile(file, newline, sizeof(newline) - 1, &written, nullptr);
  FlushFileBuffers(file);
  CloseHandle(file);
#else
  (void)message;
#endif
}

void launcherLogCount(const char* stage, std::uint32_t count) {
#ifdef ARLAND_LAUNCHER_DIAGNOSTIC
  char message[128] = { };
  wsprintfA(message, "%s count=%lu", stage,
    static_cast<unsigned long>(count));
  launcherLog(message);
#else
  (void)stage;
  (void)count;
#endif
}

constexpr std::uintptr_t TableRva = 0x1a1c88;
constexpr std::uintptr_t CodeRva = 0x0b56a;
constexpr std::uintptr_t ModeBuilderRva = 0x0b460;
constexpr std::uintptr_t CapacityImmediateRva = 0x0b508;
constexpr std::uintptr_t FallbackSizeImmediateRva = 0x0b6c9;
constexpr std::uintptr_t AllocatorRva = 0x15bcc;
constexpr DWORD LauncherImageSize = 0x317000;

constexpr std::array<std::uint8_t, 40> OriginalTable = {
  0x00,0x05,0x00,0x00, 0xd0,0x02,0x00,0x00, // 1280x720
  0x56,0x05,0x00,0x00, 0x00,0x03,0x00,0x00, // 1366x768
  0x40,0x06,0x00,0x00, 0x84,0x03,0x00,0x00, // 1600x900
  0x80,0x07,0x00,0x00, 0x38,0x04,0x00,0x00, // 1920x1080
  0x00,0x0f,0x00,0x00, 0x70,0x08,0x00,0x00, // 3840x2160
};

struct DisplayMode {
  std::uint32_t width;
  std::uint32_t height;
};

constexpr std::array<DisplayMode, 6> SupportedModes = {{
  { 1280, 720 },
  { 1366, 768 },
  { 1600, 900 },
  { 1920, 1080 },
  { 2560, 1440 },
  { 3840, 2160 },
}};

constexpr std::array<std::uint8_t, 48> TableCodeSignature = {
  0x3b,0x14,0xdd,0x88,0x1c,0x5a,0x00,0x0f,
  0x86,0xc6,0x00,0x00,0x00,0x3b,0x0c,0xdd,
  0x8c,0x1c,0x5a,0x00,0x0f,0x86,0xb9,0x00,
  0x00,0x00,0x8b,0xc3,0x89,0x5d,0xe4,0x83,
  0xfb,0x05,0x0f,0x8d,0xab,0x00,0x00,0x00,
  0x8b,0x14,0xc5,0x88,0x1c,0x5a,0x00,0x8b,
};

bool matchesModeBuilder(const std::uint8_t* code, const std::uint8_t* table) {
  // The three absolute table operands are relocated when ASLR changes the
  // launcher's image base. Validate their runtime values and compare every
  // remaining instruction byte against the shared Steam signature.
  const std::uint32_t tableAddress = static_cast<std::uint32_t>(
    reinterpret_cast<std::uintptr_t>(table));
  std::array<std::uint8_t, TableCodeSignature.size()> expected = TableCodeSignature;
  std::memcpy(expected.data() + 3, &tableAddress, sizeof(tableAddress));
  const std::uint32_t heightAddress = tableAddress + 4;
  std::memcpy(expected.data() + 16, &heightAddress, sizeof(heightAddress));
  std::memcpy(expected.data() + 43, &tableAddress, sizeof(tableAddress));
  return std::memcmp(code, expected.data(), expected.size()) == 0;
}

using PFN_AlphaBlend = BOOL (WINAPI *)(
  HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);
using PFN_TransparentBlt = BOOL (WINAPI *)(
  HDC, int, int, int, int, HDC, int, int, int, int, UINT);
INIT_ONCE g_msimg32Init = INIT_ONCE_STATIC_INIT;
PFN_AlphaBlend g_alphaBlend = nullptr;
PFN_TransparentBlt g_transparentBlt = nullptr;
using PFN_ModeBuilder = void (__thiscall *)(void*);
PFN_ModeBuilder g_modeBuilder = nullptr;

// Both 32-bit front-ends import msimg32, so one proxy is loaded into each and
// has to tell them apart: ArlandDXEnv.exe is the settings editor (it gets the
// resolution patch below), ArlandDXLauncher.exe is what Steam runs (it gets
// redirected to our own configurator). Returns the host's file name, or an
// empty string if it could not be determined.
bool hostExeName(std::array<wchar_t, 32768>& path, const wchar_t** name) {
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), path.size());
  if (!length || length == path.size())
    return false;
  *name = path.data();
  for (const wchar_t* cursor = path.data(); *cursor; cursor++) {
    if (*cursor == L'\\' || *cursor == L'/')
      *name = cursor + 1;
  }
  return true;
}

bool isSettingsEditor() {
  std::array<wchar_t, 32768> path = { };
  const wchar_t* name = nullptr;
  return hostExeName(path, &name) && _wcsicmp(name, L"ArlandDXEnv.exe") == 0;
}

void appendSupportedModesToList(std::uint32_t& count, DisplayMode* modes) {
  if (!modes)
    return;

  for (const DisplayMode& supported : SupportedModes) {
    bool found = false;
    for (std::uint32_t i = 0; i < count; i++) {
      if (modes[i].width == supported.width && modes[i].height == supported.height) {
        found = true;
        break;
      }
    }
    if (!found)
      modes[count++] = supported;
  }

  // The original builder emits ascending modes. Restore that ordering after
  // adding virtual modes above the monitor's reported maximum.
  for (std::uint32_t i = 1; i < count; i++) {
    const DisplayMode value = modes[i];
    std::uint32_t position = i;
    while (position &&
        (modes[position - 1].width > value.width ||
         (modes[position - 1].width == value.width &&
          modes[position - 1].height > value.height))) {
      modes[position] = modes[position - 1];
      position--;
    }
    modes[position] = value;
  }
}

void appendSupportedModes(void* object) {
  auto* base = reinterpret_cast<std::uint8_t*>(object);
  auto& fullscreenCount =
    *reinterpret_cast<std::uint32_t*>(base + 0x1e4);
  auto& fullscreenModes =
    *reinterpret_cast<DisplayMode**>(base + 0x1ec);
  auto& windowedCount =
    *reinterpret_cast<std::uint32_t*>(base + 0x1e8);
  auto* windowedModes =
    *reinterpret_cast<DisplayMode**>(base + 0x1f0);

  HMODULE module = GetModuleHandleW(nullptr);
  if (module) {
    using AllocateProc = void* (__cdecl *)(std::size_t);
    auto allocate = reinterpret_cast<AllocateProc>(
      reinterpret_cast<std::uint8_t*>(module) + AllocatorRva);
    auto* grown = static_cast<DisplayMode*>(allocate(
      (fullscreenCount + SupportedModes.size()) * sizeof(DisplayMode)));
    if (grown) {
      if (fullscreenModes && fullscreenCount)
        std::memcpy(grown, fullscreenModes,
          fullscreenCount * sizeof(DisplayMode));
      // The launcher's original allocation is intentionally leaked once.
      // The replacement uses its own allocator and is freed normally when
      // the short-lived settings process exits.
      fullscreenModes = grown;
      appendSupportedModesToList(fullscreenCount, fullscreenModes);
    }
  }
  appendSupportedModesToList(windowedCount, windowedModes);
}

void __fastcall modeBuilderHook(void* object, void*) {
  launcherLogCount("mode hook entered", object
    ? *reinterpret_cast<std::uint32_t*>(
        reinterpret_cast<std::uint8_t*>(object) + 0x1e8) : 0);
  g_modeBuilder(object);
  launcherLogCount("mode original returned", object
    ? *reinterpret_cast<std::uint32_t*>(
        reinterpret_cast<std::uint8_t*>(object) + 0x1e8) : 0);
  appendSupportedModes(object);
  launcherLogCount("mode append returned", object
    ? *reinterpret_cast<std::uint32_t*>(
        reinterpret_cast<std::uint8_t*>(object) + 0x1e8) : 0);
}

bool writeByte(std::uint8_t* address, std::uint8_t expected, std::uint8_t replacement) {
  if (*address != expected)
    return false;
  DWORD oldProtect = 0;
  if (!VirtualProtect(address, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
    return false;
  *address = replacement;
  DWORD ignored = 0;
  VirtualProtect(address, 1, oldProtect, &ignored);
  return true;
}

bool installModeBuilderHook(std::uint8_t* target) {
  constexpr std::array<std::uint8_t, 5> prologue = { 0x55,0x8b,0xec,0x6a,0xff };
  if (std::memcmp(target, prologue.data(), prologue.size()) != 0)
    return false;

  auto* trampoline = reinterpret_cast<std::uint8_t*>(VirtualAlloc(
    nullptr, 10, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!trampoline)
    return false;
  std::memcpy(trampoline, target, prologue.size());
  trampoline[5] = 0xe9;
  const std::int32_t returnDelta = static_cast<std::int32_t>(
    (target + 5) - (trampoline + 10));
  std::memcpy(trampoline + 6, &returnDelta, sizeof(returnDelta));

  DWORD oldProtect = 0;
  if (!VirtualProtect(target, prologue.size(), PAGE_EXECUTE_READWRITE, &oldProtect))
    return false;
  target[0] = 0xe9;
  const std::int32_t hookDelta = static_cast<std::int32_t>(
    reinterpret_cast<std::uint8_t*>(&modeBuilderHook) - (target + 5));
  std::memcpy(target + 1, &hookDelta, sizeof(hookDelta));
  DWORD ignored = 0;
  VirtualProtect(target, prologue.size(), oldProtect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), target, prologue.size());

  g_modeBuilder = reinterpret_cast<PFN_ModeBuilder>(trampoline);
  return true;
}

// ---- the launcher redirect -------------------------------------------------
//
// Steam runs ArlandDXLauncher.exe. When our own launcher is installed beside it
// we run that instead, and the stock one never puts a window on screen, so a
// plain drop-in install replaces it with no extra steps.
//
// [Launcher] SkipLauncher in arland-fix.ini turns that into a straight start of
// the game: neither front-end appears and the configured settings are used as
// they stand. Only the destination changes -- everything below about when the
// substitution happens and how long this process lives applies unchanged, which
// is what keeps the Steam session, the overlay and Steam Input attached either
// way.
//
// Two things about *when* and *for how long* this happens are load-bearing, and
// both were learned the hard way:
//
//  - It must not happen in DllMain. This DLL is a static import of the
//    launcher, so its process attach runs before the executable's entry point
//    and before anything injected into the process has finished setting itself
//    up -- including Steam's overlay, which hooks process creation in order to
//    follow the game into child processes. Starting our launcher from there
//    produced a child Steam knew nothing about: no overlay, no frame-rate
//    counter, and no Steam Input, which is what makes a DualSense work at all
//    when Steam is handling it. So the redirect is armed here and runs at the
//    executable's entry point instead, by which time the process is fully
//    assembled.
//
//  - The stock launcher process must stay alive while ours is open, rather than
//    being terminated the moment its replacement starts. It is the process
//    Steam launched and is counting, and the game is started from our launcher
//    underneath it. Waiting costs nothing -- this process has no window and no
//    work of its own once its entry point belongs to us.
//
// Nothing here is destructive if the install is partial: with no
// arland-fix-launcher.exe next to it the redirect is never armed and the stock
// launcher comes up exactly as before.
//
// ARLAND_NO_REDIRECT stands the redirect down. Our launcher sets it on the
// original launcher and settings editor when its own buttons open them, which
// is what stops those buttons from being bounced straight back here.

// What the redirect starts: our launcher normally, the game itself when
// SkipLauncher is set. `g_startsGame` only picks the wording in the log.
std::array<wchar_t, 32768> g_startTarget = { };
bool g_startsGame = false;
std::array<wchar_t, 32768> g_gameDirectory = { };
std::uint8_t* g_entryPoint = nullptr;
std::array<std::uint8_t, 5> g_entryOriginal = { };

// `directory` + `name`, where g_gameDirectory keeps its trailing backslash, so
// this is a plain concatenation. False if the result does not fit.
bool pathInGameDirectory(const wchar_t* name, std::array<wchar_t, 32768>& out) {
  const std::size_t directoryLength =
    static_cast<std::size_t>(lstrlenW(g_gameDirectory.data()));
  const std::size_t nameLength = static_cast<std::size_t>(lstrlenW(name));
  if (directoryLength + nameLength + 1 > out.size())
    return false;
  std::memcpy(out.data(), g_gameDirectory.data(),
    directoryLength * sizeof(wchar_t));
  std::memcpy(out.data() + directoryLength, name,
    (nameLength + 1) * sizeof(wchar_t));
  return true;
}

// [Launcher] SkipLauncher in arland-fix.ini, read the way the mod reads every
// other boolean (t/T/1/y/Y is true). Read wide, because the ini sits in the
// game folder and a Steam library path can hold characters the ANSI code page
// cannot represent.
//
// Deliberately read-only: unlike the DLL's own options this one is never seeded
// when it is absent. Creating arland-fix.ini from here would leave config.cpp's
// first-use seeding thinking the file already exists, and the rest of the
// defaults would never be written into it.
bool skipLauncherRequested() {
  std::array<wchar_t, 32768> ini = { };
  if (!pathInGameDirectory(L"arland-fix.ini", ini))
    return false;
  std::array<wchar_t, 16> value = { };
  GetPrivateProfileStringW(L"Launcher", L"SkipLauncher", L"false",
    value.data(), static_cast<DWORD>(value.size()), ini.data());
  return value[0] == L't' || value[0] == L'T' || value[0] == L'1' ||
         value[0] == L'y' || value[0] == L'Y';
}

struct GameExecutables {
  const wchar_t* english;
  const wchar_t* multilingual;
};

// The three games, under the same executable names the mod's own launcher
// matches on (kGames in src/config_gui/main.cpp). Each ships as two builds, an
// English one and a multilingual one carrying Japanese, Simplified Chinese and
// Traditional Chinese, normally installed side by side.
constexpr std::array<GameExecutables, 3> SupportedGames = {{
  { L"A11R_x64_Release_en.exe", L"A11R_x64_Release.exe" },
  { L"A12V_x64_Release_en.exe", L"A12V_x64_Release.exe" },
  { L"A13V_x64_Release_EN.exe", L"A13V_x64_Release.exe" },
}};

// Which executable a straight start runs, decided exactly as Koei Tecmo's
// launcher and our own decide it: [Lang] Language in ArlandDX_Settings.ini
// selects the multilingual build for 1 (Japanese), 3 (Simplified Chinese) and 4
// (Traditional Chinese), and the English build for anything else. The two
// builds do not each carry every language, so this matters. If the build the
// language calls for is not installed the other one is used rather than
// starting nothing.
bool resolveGameExecutable(std::array<wchar_t, 32768>& out) {
  std::array<wchar_t, 16> language = { };
  std::array<wchar_t, 32768> settings = { };
  if (pathInGameDirectory(L"ArlandDX_Settings.ini", settings))
    GetPrivateProfileStringW(L"Lang", L"Language", L"2", language.data(),
      static_cast<DWORD>(language.size()), settings.data());
  const bool english = language[0] != L'1' && language[0] != L'3' &&
                       language[0] != L'4';

  for (const GameExecutables& game : SupportedGames) {
    const wchar_t* candidates[2] = {
      english ? game.english : game.multilingual,
      english ? game.multilingual : game.english,
    };
    for (const wchar_t* name : candidates) {
      if (pathInGameDirectory(name, out) &&
          GetFileAttributesW(out.data()) != INVALID_FILE_ATTRIBUTES)
        return true;
    }
  }
  out[0] = L'\0';
  return false;
}

// Put the executable's own entry point back and run it, for the case where the
// target cannot be started after all. The launcher then comes up as if the mod
// were not installed.
void runOriginalEntryPoint() {
  DWORD oldProtect = 0;
  if (VirtualProtect(g_entryPoint, g_entryOriginal.size(),
      PAGE_EXECUTE_READWRITE, &oldProtect)) {
    std::memcpy(g_entryPoint, g_entryOriginal.data(), g_entryOriginal.size());
    DWORD ignored = 0;
    VirtualProtect(g_entryPoint, g_entryOriginal.size(), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), g_entryPoint,
      g_entryOriginal.size());
  }
  reinterpret_cast<void (*)()>(g_entryPoint)();
}

// Stands in for the launcher's entry point once the redirect is armed.
void redirectedEntryPoint() {
  STARTUPINFOW startup = { };
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = { };
  // Both targets are 64-bit and this proxy is 32-bit; CreateProcess spans that
  // difference, and the child inherits our environment either way -- which is
  // how the Steam variables reach the game and stop it restarting itself
  // through Steam. Starting the game from here rather than from our launcher
  // makes it a child of this process instead of a grandchild, which is the
  // same relationship our launcher gives it and the one Steam follows.
  if (!CreateProcessW(g_startTarget.data(), nullptr, nullptr, nullptr, FALSE,
      0, nullptr, g_gameDirectory.data(), &startup, &process)) {
    launcherLog(g_startsGame
      ? "the game failed to start; running the stock launcher"
      : "configurator failed to start; running the stock launcher");
    runOriginalEntryPoint();
    return;
  }
  CloseHandle(process.hThread);
  launcherLog(g_startsGame
    ? "game started; holding this process open behind it"
    : "configurator started; holding this process open behind it");
  WaitForSingleObject(process.hProcess, INFINITE);
  CloseHandle(process.hProcess);
  launcherLog(g_startsGame
    ? "game closed; ending the stock launcher"
    : "configurator closed; ending the stock launcher");
  ExitProcess(0);
}

// Point the executable's entry point at redirectedEntryPoint. Returns false
// with the image untouched if anything does not look as expected.
bool armRedirect() {
  std::array<wchar_t, 32768> path = { };
  const wchar_t* name = nullptr;
  if (!hostExeName(path, &name))
    return false;
  if (_wcsicmp(name, L"ArlandDXLauncher.exe") != 0)
    return false;

  if (GetEnvironmentVariableW(L"ARLAND_NO_REDIRECT", nullptr, 0) != 0 ||
      GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
    launcherLog("redirect stood down by ARLAND_NO_REDIRECT");
    return false;
  }

  // Directory of the launcher, which is also the game folder our launcher is
  // dropped into. `name` points into `path`, so truncating there leaves the
  // directory with its trailing backslash.
  const std::size_t directoryLength = static_cast<std::size_t>(name - path.data());
  std::memcpy(g_gameDirectory.data(), path.data(),
    directoryLength * sizeof(wchar_t));

  // Where the redirect goes. SkipLauncher asks for the game itself; without it
  // (the default) this is our launcher, exactly as before.
  g_startsGame = skipLauncherRequested();
  if (g_startsGame) {
    if (!resolveGameExecutable(g_startTarget)) {
      launcherLog("SkipLauncher is set but no game executable is installed "
        "here; leaving the stock launcher alone");
      return false;
    }
  } else if (!pathInGameDirectory(L"arland-fix-launcher.exe", g_startTarget) ||
      GetFileAttributesW(g_startTarget.data()) == INVALID_FILE_ATTRIBUTES) {
    launcherLog("no configurator installed; leaving the stock launcher alone");
    return false;
  }

  auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
  if (!base)
    return false;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return false;
  const auto* nt =
    reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
      !nt->OptionalHeader.AddressOfEntryPoint)
    return false;

  // Taken from the headers rather than from a hardcoded address, so this holds
  // for all three games' launchers and for any future build of them.
  g_entryPoint = base + nt->OptionalHeader.AddressOfEntryPoint;
  std::memcpy(g_entryOriginal.data(), g_entryPoint, g_entryOriginal.size());

  DWORD oldProtect = 0;
  if (!VirtualProtect(g_entryPoint, g_entryOriginal.size(),
      PAGE_EXECUTE_READWRITE, &oldProtect)) {
    launcherLog("entry point is not writable; leaving the stock launcher");
    return false;
  }
  g_entryPoint[0] = 0xe9;
  const std::int32_t delta = static_cast<std::int32_t>(
    reinterpret_cast<std::uint8_t*>(&redirectedEntryPoint) - (g_entryPoint + 5));
  std::memcpy(g_entryPoint + 1, &delta, sizeof(delta));
  DWORD ignored = 0;
  VirtualProtect(g_entryPoint, g_entryOriginal.size(), oldProtect, &ignored);
  FlushInstructionCache(GetCurrentProcess(), g_entryPoint,
    g_entryOriginal.size());
  launcherLog(g_startsGame
    ? "redirect armed at the launcher entry point (straight to the game)"
    : "redirect armed at the launcher entry point");
  return true;
}

void installLauncherResolutionHook() {
  launcherLog("patch initialization entered");
  HMODULE module = GetModuleHandleW(nullptr);
  if (!module || !isSettingsEditor()) {
    launcherLog("process is forwarding-only");
    return;
  }
  launcherLog("launcher process recognized");

  auto* base = reinterpret_cast<std::uint8_t*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    launcherLog("DOS header failed");
    return;
  }
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
      nt->OptionalHeader.SizeOfImage != LauncherImageSize) {
    launcherLog("PE header failed");
    return;
  }
  launcherLog("PE header passed");

  auto* table = base + TableRva;
  const std::array<std::uint8_t, 5> allocatorExpected = {
    0x55, 0x8b, 0xec, 0x5d, 0xe9,
  };
  if (!matchesModeBuilder(base + CodeRva, table) ||
      std::memcmp(table, OriginalTable.data(), OriginalTable.size()) != 0 ||
      std::memcmp(base + AllocatorRva, allocatorExpected.data(),
        allocatorExpected.size()) != 0) {
    launcherLog("table/code signatures failed");
    return;
  }
  launcherLog("table/code signatures passed");

  // The original reserves detected-mode count + 5 entries. We expose six
  // canonical modes, so increase both normal and no-DXGI-mode allocations.
  if (!writeByte(base + CapacityImmediateRva, 0x05, 0x06)) {
    launcherLog("capacity patch failed");
    return;
  }
  launcherLog("capacity patch passed");
  if (!writeByte(base + FallbackSizeImmediateRva, 0x28, 0x30)) {
    launcherLog("fallback patch failed");
    return;
  }
  launcherLog("fallback patch passed");
  launcherLog(installModeBuilderHook(base + ModeBuilderRva)
    ? "mode hook installed" : "mode hook failed");
}

BOOL CALLBACK loadSystemMsimg32(PINIT_ONCE, PVOID, PVOID*) {
  std::array<wchar_t, MAX_PATH> path = { };
  const UINT length = GetSystemDirectoryW(path.data(), path.size());
  if (!length || length + 14 >= path.size())
    return TRUE;
  std::memcpy(path.data() + length, L"\\msimg32.dll", 13 * sizeof(wchar_t));
  HMODULE module = LoadLibraryW(path.data());
  if (module) {
    g_alphaBlend = reinterpret_cast<PFN_AlphaBlend>(
      GetProcAddress(module, "AlphaBlend"));
    g_transparentBlt = reinterpret_cast<PFN_TransparentBlt>(
      GetProcAddress(module, "TransparentBlt"));
  }
  launcherLog(module && g_alphaBlend && g_transparentBlt
    ? "system msimg32 forwarding ready"
    : "system msimg32 forwarding failed");
  return TRUE;
}

} // namespace

extern "C" BOOL WINAPI AlphaBlend(
    HDC dst, int dstX, int dstY, int dstWidth, int dstHeight,
    HDC src, int srcX, int srcY, int srcWidth, int srcHeight,
    BLENDFUNCTION blend) {
  InitOnceExecuteOnce(&g_msimg32Init, loadSystemMsimg32, nullptr, nullptr);
  return g_alphaBlend && g_alphaBlend(dst, dstX, dstY, dstWidth, dstHeight,
    src, srcX, srcY, srcWidth, srcHeight, blend);
}

extern "C" BOOL WINAPI TransparentBlt(
    HDC dst, int dstX, int dstY, int dstWidth, int dstHeight,
    HDC src, int srcX, int srcY, int srcWidth, int srcHeight,
    UINT transparent) {
  InitOnceExecuteOnce(&g_msimg32Init, loadSystemMsimg32, nullptr, nullptr);
  return g_transparentBlt && g_transparentBlt(
    dst, dstX, dstY, dstWidth, dstHeight,
    src, srcX, srcY, srcWidth, srcHeight, transparent);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    launcherLog("msimg32 process attach");
    // Only armed here; it runs at the executable's entry point, once the
    // process (Steam's injections included) is fully assembled.
    if (armRedirect())
      return TRUE;
    installLauncherResolutionHook();
  }
  return TRUE;
}
