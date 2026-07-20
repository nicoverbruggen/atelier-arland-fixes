# Session handoff — 2026-07-20

Read `AGENTS.md`, then `TECHNICAL.md` and `TODO.md`. The broader reverse-engineering record remains in `../REPORT.md`.

## Current release candidate

The current release builds and publishes the 64-bit game `d3d11.dll` and the 32-bit settings-launcher `msimg32.dll`. Clean local builds are `builds/release-x64/d3d11.dll`, SHA-256 `d801be808b8848431f4b5c1121838a1724e8365d7bae60e89d49f1a751c8ca28`, and `builds/release-x86/msimg32.dll`, SHA-256 `39469b779029a2a595eabefbdd6fed8639bd1b7422bd3c1e6b437106a23a8569`.

The game DLL includes the Rorona, Totori, and Meruru English menu fixes, the D3D11 synchronization/coherence work, and the existing game-side 1440p/4K target and raster correction. `arland-fix.ini` is created with MSAA disabled and blank resolution overrides. The launcher DLL guarantees 1080p, 1440p, and 4K in both its independent fullscreen and windowed mode arrays and forwards `AlphaBlend` and `TransparentBlt` to the system MSIMG32 library. WinMM was rejected as the proxy boundary because its incomplete export surface caused native DirectX initialization to fail before the mode hook ran. Native Windows testing confirmed clean text, 4× MSAA, stable mode lists across the fullscreen toggle, and a successful 4K game launch. Higher-resolution blurred dialogue backgrounds remain confined to a 1920×1080 upper-left region; this is documented as a known limitation rather than treated as fixed.

## Native-Windows text-corruption result

Five native-Windows Rorona tests isolated the scrambled-text defect. The source shadow-copy optimization was creating or using a stale shadow for dynamic 512×512 font atlases whose contents had been initialized through deferred game work. Later operations remained internally coherent but coherent with the stale pixels.

The release fix is narrow: `tryCpuCopy` returns to native D3D11 behavior when its source is a dynamic 512×512 texture. The queue-scoped game-code atlas cache remains enabled and still removes thousands of redundant reads. Native Windows testing confirmed that this eliminates scrambled text while retaining the menu performance improvement.

The original logs remain in `/var/home/nico/Downloads/atfix.log` and `atfix-2.log` through `atfix-5.log`. The decisive detailed run is `atfix-5.log`: a representative 264.484 ms queue drain spent approximately 0.52 ms in cached/real PSSG and atlas work combined, leaving 263.964 ms in the game's UI construction path. Broader asset caching therefore cannot materially reduce the residual pause.

## Next performance investigation

Use the isolated Rorona executable under Proton rather than iterating on Windows. Instrument the verified per-record processor and inner layout routines with recursion-aware exclusive timing. Existing reverse-engineering in `../atelier-sync-fix/impl.cpp` identifies Rorona's record processor at RVA `0x0a5f40`; earlier probes found type 0 and type 19 expensive and the helper at `0x0a2ef0` responsible for roughly 40–42 ms on the desktop workload. Integrate timing into the existing queue-drain hook instead of detouring that boundary twice. Keep all new profiling opt-in and do not ship behavioral caching until ownership and invalidation are understood.

## Ayesha finding

Ayesha has the same queue-scoped 512×512 atlas-read architecture but does not have the repeated `.PSSG` validation problem. English executable fingerprint: SHA-256 `b10a07e494abfb5f5711aeb9151b894662b105a3478ac245bf38604d5a6e360d`, `.text` size `0x984df4`, queue drain RVA `0x078320`, text renderer `0x74bd90`, atlas lock `0x581420`, atlas unlock `0x581460`. Add only an exact-signature-gated atlas diagnostic first, measure cache-off versus cache-on, and do not install the Arland PSSG hook.

## Build

```sh
distrobox enter atfix-build -- bash -lc 'cd /var/home/nico/Desktop/Other/atelier/atelier-arland-fixes && meson setup builds/release-x64 --cross-file build-win64.txt --buildtype release && ninja -C builds/release-x64'
distrobox enter atfix-build -- bash -lc 'cd /var/home/nico/Desktop/Other/atelier/atelier-arland-fixes && meson setup builds/release-x86 --cross-file build-win32.txt --buildtype release && ninja -C builds/release-x86'
```

Build directories belong under ignored `builds/`. Do not reset or restore the current staged work; inspect the index before editing.
