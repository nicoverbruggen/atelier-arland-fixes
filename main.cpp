#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

namespace {

using CreateDeviceProc = HRESULT (WINAPI*)(
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
  const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
  D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using CreateDeviceAndSwapChainProc = HRESULT (WINAPI*)(
  IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
  const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*,
  IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
  ID3D11DeviceContext**);
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

CreateDeviceProc realCreateDevice = nullptr;
CreateDeviceAndSwapChainProc realCreateDeviceAndSwapChain = nullptr;
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
  const char* disabled = std::getenv("ATFIX_NO_PSSG_PATH_CACHE");
  const char* enabled = std::getenv("ATFIX_PSSG_PATH_CACHE");
  if ((disabled && disabled[0] == '1') || (enabled && enabled[0] == '0'))
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
    path += L"\\d3d11.dll";
    HMODULE systemD3D11 = LoadLibraryW(path.c_str());
    if (!systemD3D11)
      return;
    realCreateDevice = reinterpret_cast<CreateDeviceProc>(
      GetProcAddress(systemD3D11, "D3D11CreateDevice"));
    realCreateDeviceAndSwapChain = reinterpret_cast<CreateDeviceAndSwapChainProc>(
      GetProcAddress(systemD3D11, "D3D11CreateDeviceAndSwapChain"));
    installMenuFix();
  });
}

} // namespace

extern "C" HRESULT WINAPI D3D11CreateDevice(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount,
    UINT sdkVersion, ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel,
    ID3D11DeviceContext** immediateContext) {
  initialize();
  return realCreateDevice
    ? realCreateDevice(adapter, driverType, software, flags, featureLevels,
        featureLevelCount, sdkVersion, device, featureLevel, immediateContext)
    : E_FAIL;
}

extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount,
    UINT sdkVersion, const DXGI_SWAP_CHAIN_DESC* swapChainDesc,
    IDXGISwapChain** swapChain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** immediateContext) {
  initialize();
  return realCreateDeviceAndSwapChain
    ? realCreateDeviceAndSwapChain(adapter, driverType, software, flags,
        featureLevels, featureLevelCount, sdkVersion, swapChainDesc, swapChain,
        device, featureLevel, immediateContext)
    : E_FAIL;
}
