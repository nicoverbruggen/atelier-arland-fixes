// Launcher-only 32-bit WinMM proxy. Original project code under the MIT terms
// in ../LICENSE; it modifies only the verified process image in memory.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace {

constexpr std::uintptr_t TableRva = 0x1a1c88;
constexpr std::uintptr_t CodeRva = 0x0b56a;
constexpr std::uintptr_t ModeBuilderRva = 0x0b460;
constexpr std::uintptr_t CapacityImmediateRva = 0x0b508;
constexpr std::uintptr_t FallbackSizeImmediateRva = 0x0b6c9;
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

using PFN_PlaySoundW = BOOL (WINAPI *)(LPCWSTR, HMODULE, DWORD);
INIT_ONCE g_winmmInit = INIT_ONCE_STATIC_INIT;
PFN_PlaySoundW g_playSoundW = nullptr;
using PFN_ModeBuilder = void (__thiscall *)(void*);
PFN_ModeBuilder g_modeBuilder = nullptr;

bool isLauncher(HMODULE module) {
  std::array<wchar_t, 32768> path = { };
  const DWORD length = GetModuleFileNameW(module, path.data(), path.size());
  if (!length || length == path.size())
    return false;
  const wchar_t* name = path.data();
  for (const wchar_t* cursor = path.data(); *cursor; cursor++) {
    if (*cursor == L'\\' || *cursor == L'/')
      name = cursor + 1;
  }
  return _wcsicmp(name, L"ArlandDXEnv.exe") == 0;
}

void appendSupportedModes(void* object) {
  auto* base = reinterpret_cast<std::uint8_t*>(object);
  auto& count = *reinterpret_cast<std::uint32_t*>(base + 0x1e8);
  auto* modes = *reinterpret_cast<DisplayMode**>(base + 0x1f0);
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

void __fastcall modeBuilderHook(void* object, void*) {
  g_modeBuilder(object);
  appendSupportedModes(object);
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

void installLauncherResolutionHook() {
  HMODULE module = GetModuleHandleW(nullptr);
  if (!module || !isLauncher(module))
    return;

  auto* base = reinterpret_cast<std::uint8_t*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
      nt->OptionalHeader.SizeOfImage != LauncherImageSize)
    return;

  auto* table = base + TableRva;
  if (!matchesModeBuilder(base + CodeRva, table) ||
      std::memcmp(table, OriginalTable.data(), OriginalTable.size()) != 0)
    return;

  // The original reserves detected-mode count + 5 entries. We expose six
  // canonical modes, so increase both normal and no-DXGI-mode allocations.
  if (!writeByte(base + CapacityImmediateRva, 0x05, 0x06) ||
      !writeByte(base + FallbackSizeImmediateRva, 0x28, 0x30))
    return;
  installModeBuilderHook(base + ModeBuilderRva);
}

BOOL CALLBACK loadSystemWinmm(PINIT_ONCE, PVOID, PVOID*) {
  std::array<wchar_t, MAX_PATH> path = { };
  const UINT length = GetSystemDirectoryW(path.data(), path.size());
  if (!length || length + 12 >= path.size())
    return TRUE;
  std::memcpy(path.data() + length, L"\\winmm.dll", 11 * sizeof(wchar_t));
  HMODULE module = LoadLibraryW(path.data());
  if (module)
    g_playSoundW = reinterpret_cast<PFN_PlaySoundW>(GetProcAddress(module, "PlaySoundW"));
  return TRUE;
}

} // namespace

extern "C" BOOL WINAPI PlaySoundW(LPCWSTR sound, HMODULE module, DWORD flags) {
  InitOnceExecuteOnce(&g_winmmInit, loadSystemWinmm, nullptr, nullptr);
  return g_playSoundW ? g_playSoundW(sound, module, flags) : FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    installLauncherResolutionHook();
  }
  return TRUE;
}
