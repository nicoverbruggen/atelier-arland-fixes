# Technical explanation

## The bug

The Arland DX games store menu graphics and localized UI data in immutable `.PSSG` archives. While constructing a menu, the English executables recursively visit many text and UI records. Those visits repeatedly ask an internal helper whether the same archive path is valid, even after that path has already succeeded earlier in the process.

This is not repeated reading of the archive contents. In Rorona and Meruru, each successful validation performs both a CRT-style metadata query and a filename-case check implemented with a directory enumeration. Under Wine, the resulting work appears as repeated file opens, metadata queries, directory queries, and closes against the same few paths. Totori uses a related validation helper that repeats the metadata lookup but does not perform the same `FindFirstFile` case scan.

Rorona exposes the logic error most clearly. Its resource bookkeeping remembers failed archive checks, but does not mark successful checks as complete. Recursive UI construction therefore validates a successful path again for every visit. During one Rorona Status-menu transition, the unmodified English executable validated just `a11r_menu_EN.PSSG` and `a11r_common_EN.PSSG` 1,245 redundant times.

The operating system's file cache does not eliminate this cost. Cached file contents do not make repeated opens, metadata requests, directory enumeration, path conversion, Wine/NT syscall handling, and handle teardown free. Preloading every PSSG archive would not address the bug either: genuine archive reads already occur only around first use and measured roughly 65–140 microseconds, while the expensive work was repeated path validation.

These desktop measurements were captured on a high-end system with an AMD Ryzen 7 5800X3D, Radeon RX 7900 XTX, and 32 GiB of RAM. The unmodified behavior is substantially worse on Steam Deck, where affected menu opens were observed taking roughly 4–7 seconds each.

| Rorona menu test | Without complete cache | With complete cache | Reduction |
|---|---:|---:|---:|
| Status menu open | 916.6 ms | 103.1 ms | 88.8% |
| Quest, Container, and Basket menu test | 632–711 ms | 135.6 ms | 78.5–80.9% |

The Quest, Container, and Basket figure is the slowest menu transition recorded while exercising those three previously problematic menus. The Status result preserved the same UI record counts and rendered output. Caching only the low-level metadata result reached 728.5 ms, demonstrating that most of the saving comes from bypassing the complete redundant validation path, including the filename-case directory scan.

## The fix

This DLL detours the games' high-level path-validation helper, not their archive loader or general Windows file APIs. The first time a `.PSSG` path is encountered, the original game function runs unchanged. If it succeeds, the complete path string is added to a process-local set. Later checks of that exact path return success immediately.

Only successful `.PSSG` validations are cached. Failed checks always reach the original function, so a resource that becomes available later can still be discovered. Non-PSSG paths always reach the original function. Actual archive opening, reading, decompression, parsing, ownership, and lifetime remain entirely under the game's control. Cache access is protected by a mutex because menu population can involve multiple threads.

The cache deliberately stores validation results rather than PSSG bytes or completed UI objects. The archives do not change during normal play, so a previously successful validation remains valid for the lifetime of the game process. Reusing a completed UI graph would be unsafe because those nodes are mutable and owned by the active menu; skipping only redundant validation leaves that ownership model untouched.

## Hook safety

The mod is a `dinput8.dll` proxy. It loads the real system `dinput8.dll`, forwards `DirectInput8Create`, and installs the menu hook during initialization. This lets it live beside a separate `d3d11.dll` mod such as TellowKrinkle's atelier-sync-fix.

Each supported game has its own known path-check function address. The hook is installed only when the executable filename, `.text` section size, and the first 18 bytes of the expected function prologue all match. Unknown or modified builds are left untouched. The detour copies those verified whole instructions into a trampoline and uses an absolute x64 jump to the cache wrapper; no instruction is split.

The supported entry points are:

| Game | Executable | Path-check RVA | Validation behavior |
|---|---|---:|---|
| Rorona DX | `A11R_x64_Release_en.exe` | `0x12cc70` | metadata query plus filename-case directory scan |
| Totori DX | `A12V_x64_Release_en.exe` | `0x18b140` | repeated metadata validation |
| Meruru DX | `A13V_x64_Release_EN.exe` | `0x1533c0` | metadata query plus filename-case directory scan |

The fix has a narrow contract: remember immutable PSSG paths that the game has already validated successfully, and change nothing else.
