#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

namespace {

using DirectInput8CreateProc = HRESULT (WINAPI*)(
  HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using PathCheckProc = bool (*)(void*, void*);

struct Game {
  const char* executable;
  DWORD textSize;
  uintptr_t pathCheckRva;
  BYTE pushedRegister;
};

constexpr Game games[] = {
  { "A11R_x64_Release_en.exe", 0x709a9c, 0x12cc70, 0x57 },
  { "A12V_x64_Release_en.exe", 0x67da5c, 0x18b140, 0x56 },
  { "A13V_x64_Release_EN.exe", 0x61ecec, 0x1533c0, 0x57 },
};

DirectInput8CreateProc realDirectInput8Create = nullptr;
PathCheckProc originalPathCheck = nullptr;
std::once_flag initialization;
std::mutex cacheMutex;
std::unordered_set<std::string> successfulPaths;

const char* baseName(const char* path) {
  const char* back = std::strrchr(path, '\\');
  const char* forward = std::strrchr(path, '/');
  const char* slash = !back || (forward && forward > back) ? forward : back;
  return slash ? slash + 1 : path;
}

DWORD textSectionSize(HMODULE module) {
  auto* base = reinterpret_cast<BYTE*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return 0;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return 0;
  auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
    if (!std::memcmp(section->Name, ".text", 5))
      return section->Misc.VirtualSize;
  }
  return 0;
}

const char* pssgPath(const void* stringObject) {
  if (!stringObject)
    return nullptr;
  const auto* bytes = static_cast<const BYTE*>(stringObject);
  size_t capacity = 0;
  std::memcpy(&capacity, bytes + 0x18, sizeof(capacity));
  const char* path = nullptr;
  if (capacity >= 0x10)
    std::memcpy(&path, bytes, sizeof(path));
  else
    path = static_cast<const char*>(stringObject);
  if (!path)
    return nullptr;
  const char* extension = std::strrchr(path, '.');
  return extension && !_stricmp(extension, ".PSSG") ? path : nullptr;
}

bool cachedPathCheck(void* context, void* pathString) {
  const char* path = pssgPath(pathString);
  if (!path)
    return originalPathCheck(context, pathString);
  const std::string key(path);
  {
    std::lock_guard lock(cacheMutex);
    if (successfulPaths.find(key) != successfulPaths.end())
      return true;
  }
  if (!originalPathCheck(context, pathString))
    return false;
  std::lock_guard lock(cacheMutex);
  successfulPaths.insert(key);
  return true;
}

void writeAbsoluteJump(BYTE* destination, const void* target) {
  destination[0] = 0xff;
  destination[1] = 0x25;
  std::memset(destination + 2, 0, 4);
  const uintptr_t address = reinterpret_cast<uintptr_t>(target);
  std::memcpy(destination + 6, &address, sizeof(address));
}

bool installDetour(BYTE* target) {
  constexpr size_t patchSize = 18;
  auto* trampoline = static_cast<BYTE*>(VirtualAlloc(
    nullptr, patchSize + 14, MEM_COMMIT | MEM_RESERVE,
    PAGE_EXECUTE_READWRITE));
  if (!trampoline)
    return false;
  std::memcpy(trampoline, target, patchSize);
  writeAbsoluteJump(trampoline + patchSize, target + patchSize);
  FlushInstructionCache(GetCurrentProcess(), trampoline, patchSize + 14);
  originalPathCheck = reinterpret_cast<PathCheckProc>(trampoline);

  DWORD oldProtection = 0;
  if (!VirtualProtect(target, patchSize, PAGE_EXECUTE_READWRITE, &oldProtection))
    return false;
  writeAbsoluteJump(target, reinterpret_cast<void*>(&cachedPathCheck));
  std::memset(target + 14, 0x90, patchSize - 14);
  FlushInstructionCache(GetCurrentProcess(), target, patchSize);
  DWORD ignored = 0;
  VirtualProtect(target, patchSize, oldProtection, &ignored);
  return true;
}

void installMenuFix() {
  const char* enabled = std::getenv("ARLAND_MENU_FIX");
  if (enabled && enabled[0] == '0')
    return;

  HMODULE module = GetModuleHandleW(nullptr);
  char imagePath[MAX_PATH] = {};
  if (!module || !GetModuleFileNameA(module, imagePath, sizeof(imagePath)))
    return;
  const DWORD textSize = textSectionSize(module);
  for (const auto& game : games) {
    if (_stricmp(baseName(imagePath), game.executable) || textSize != game.textSize)
      continue;
    auto* target = reinterpret_cast<BYTE*>(module) + game.pathCheckRva;
    const std::array<BYTE, 18> expected = {
      0x40, game.pushedRegister, 0x48, 0x81, 0xec, 0xc0, 0x00, 0x00,
      0x00, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff,
    };
    if (!std::memcmp(target, expected.data(), expected.size()))
      installDetour(target);
    return;
  }
}

void initialize() {
  std::call_once(initialization, [] {
    wchar_t systemDirectory[MAX_PATH] = {};
    if (!GetSystemDirectoryW(systemDirectory, MAX_PATH))
      return;
    std::wstring path(systemDirectory);
    path += L"\\dinput8.dll";
    HMODULE systemDinput8 = LoadLibraryW(path.c_str());
    if (!systemDinput8)
      return;
    realDirectInput8Create = reinterpret_cast<DirectInput8CreateProc>(
      GetProcAddress(systemDinput8, "DirectInput8Create"));
    installMenuFix();
  });
}

} // namespace

extern "C" HRESULT WINAPI DirectInput8Create(
    HINSTANCE instance, DWORD version, REFIID interfaceId,
    LPVOID* output, LPUNKNOWN outer) {
  initialize();
  return realDirectInput8Create
    ? realDirectInput8Create(instance, version, interfaceId, output, outer)
    : E_FAIL;
}
