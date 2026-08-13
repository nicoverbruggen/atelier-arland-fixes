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
//   ArlandDXEnv.exe      -> nothing; the settings editor is only forwarded to.
//                           Resolution belongs to our own launcher ([Rendering]
//                           DisplayWidth/DisplayHeight, applied by the 64-bit
//                           DLL), so this process needs no patching.
//
// Everything else that loads it just gets the two forwarded GDI entry points.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>


namespace {

using PFN_AlphaBlend = BOOL (WINAPI *)(
  HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);
using PFN_TransparentBlt = BOOL (WINAPI *)(
  HDC, int, int, int, int, HDC, int, int, int, int, UINT);
INIT_ONCE g_msimg32Init = INIT_ONCE_STATIC_INIT;
PFN_AlphaBlend g_alphaBlend = nullptr;
PFN_TransparentBlt g_transparentBlt = nullptr;

// Both 32-bit front-ends import msimg32, so one proxy is loaded into each and
// has to tell them apart: ArlandDXLauncher.exe is what Steam runs and is the
// only host that gets redirected to our own configurator; ArlandDXEnv.exe, the
// settings editor, is forwarded to and nothing more. Returns the host's file
// name, or an empty string if it could not be determined.
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

// All three installed ArlandDXLauncher.exe copies have byte-identical .text
// sections (their resources make the complete files differ). This is the
// verified 17-byte entry window at RVA 0x120be6, checked against every shipped
// launcher. Its final dword is an absolute address and every launcher's PE
// relocation table names it as HIGHLOW at entry+13, so the loader legitimately
// changes those four bytes when ASLR moves the image. Header-derived location
// is not identity: compare the exact loader-adjusted window before replacing
// its first five bytes.
constexpr std::array<std::uint8_t, 17> kLauncherEntryExpected = {
  0xe8, 0x08, 0xde, 0x00, 0x00, 0xe9, 0x00, 0x00,
  0x00, 0x00, 0x6a, 0x14, 0x68, 0x28, 0x8a, 0x59, 0x00,
};

constexpr std::size_t kLauncherEntryRelocationOffset = 13;
constexpr std::uint32_t kLauncherPreferredImageBase = 0x00400000;

template<std::size_t N>
void relocateEntryWindow(std::uint8_t* loadedBase,
                         std::array<std::uint8_t, N>& expected) {
  static_assert(N >= kLauncherEntryRelocationOffset + sizeof(std::uint32_t));
  std::uint32_t absolute = 0;
  std::memcpy(&absolute, expected.data() + kLauncherEntryRelocationOffset,
              sizeof(absolute));
  // PE32 HIGHLOW relocation arithmetic is modulo 2^32. Expressing the delta
  // as unsigned also covers an image loaded below its preferred base. Use the
  // verified file ImageBase rather than the loaded header: Wine rewrites that
  // header field to the actual base, which would incorrectly make this delta
  // zero under Proton.
  absolute += static_cast<std::uint32_t>(
                reinterpret_cast<std::uintptr_t>(loadedBase)) -
              kLauncherPreferredImageBase;
  std::memcpy(expected.data() + kLauncherEntryRelocationOffset, &absolute,
              sizeof(absolute));
}

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
//
// The comparison is against the whole value because the stock launcher's own
// is: it compares the parsed string against the constants "1", "2", "3" and
// "4", so "10" is unrecognized there and falls to English, where a
// first-character test would read it as Japanese.
bool resolveGameExecutable(std::array<wchar_t, 32768>& out) {
  std::array<wchar_t, 16> language = { };
  std::array<wchar_t, 32768> settings = { };
  if (pathInGameDirectory(L"ArlandDX_Settings.ini", settings))
    GetPrivateProfileStringW(L"Lang", L"Language", L"2", language.data(),
      static_cast<DWORD>(language.size()), settings.data());
  const bool english = lstrcmpW(language.data(), L"1") != 0 &&
                       lstrcmpW(language.data(), L"3") != 0 &&
                       lstrcmpW(language.data(), L"4") != 0;

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
  if (!VirtualProtect(g_entryPoint, g_entryOriginal.size(),
      PAGE_EXECUTE_READWRITE, &oldProtect)) {
    // The entry point still holds the redirect jump, so calling it would land
    // back in redirectedEntryPoint, whose failure path is this function, and
    // recurse until the stack runs out. With the restore refused there is no
    // way left to run the stock launcher; exit instead.
    ExitProcess(1);
  }
  std::memcpy(g_entryPoint, g_entryOriginal.data(), g_entryOriginal.size());
  FlushInstructionCache(GetCurrentProcess(), g_entryPoint,
    g_entryOriginal.size());
  // Unchecked, and it has to stay that way: the original bytes are already
  // back, so the stock launcher runs correctly whether or not the page returns
  // to its old protection. Refusing to call it over a left-writable page would
  // turn a cosmetic failure into a launcher that does nothing.
  DWORD ignored = 0;
  VirtualProtect(g_entryPoint, g_entryOriginal.size(), oldProtect, &ignored);
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
    runOriginalEntryPoint();
    return;
  }
  CloseHandle(process.hThread);
  WaitForSingleObject(process.hProcess, INFINITE);
  CloseHandle(process.hProcess);
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
      return false;
    }
  } else if (!pathInGameDirectory(L"arland-fix-launcher.exe", g_startTarget) ||
      GetFileAttributesW(g_startTarget.data()) == INVALID_FILE_ATTRIBUTES) {
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
      !nt->OptionalHeader.AddressOfEntryPoint ||
      nt->OptionalHeader.SizeOfImage < kLauncherEntryExpected.size() ||
      nt->OptionalHeader.AddressOfEntryPoint >
        nt->OptionalHeader.SizeOfImage - kLauncherEntryExpected.size())
    return false;

  // The header locates the entry, and the loader-adjusted shipped byte window
  // identifies it. Comparing the raw file bytes here would reject a valid
  // ASLR-rebased launcher because entry+13 is a PE HIGHLOW relocation.
  g_entryPoint = base + nt->OptionalHeader.AddressOfEntryPoint;
  auto expectedEntry = kLauncherEntryExpected;
  relocateEntryWindow(base, expectedEntry);
  if (std::memcmp(g_entryPoint, expectedEntry.data(), expectedEntry.size())) {
    g_entryPoint = nullptr;
    return false;
  }
  std::memcpy(g_entryOriginal.data(), g_entryPoint, g_entryOriginal.size());

  DWORD oldProtect = 0;
  if (!VirtualProtect(g_entryPoint, g_entryOriginal.size(),
      PAGE_EXECUTE_READWRITE, &oldProtect)) {
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
  return true;
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
    // Only armed here; it runs at the executable's entry point, once the
    // process (Steam's injections included) is fully assembled. Every other
    // host, ArlandDXEnv.exe included, is left completely alone and just gets
    // the two forwarded GDI entry points.
    armRedirect();
  }
  return TRUE;
}
