# WoWVR

SteamVR support for a 3.3.5a (12340) World of Warcraft client, aimed at the Valve Index.

The world is rendered stereoscopically around you and the headset drives the view. Movement,
targeting and everything else stay on mouse and keyboard — there is no motion-controller input.
The game UI is lifted out of the eye buffer and hung in space as a body-locked panel.

It works as a proxy `d3d9.dll` that the client loads out of its own folder. Nothing in the client
is patched on disk.

## Status

| Phase | What it covers | State |
|-------|----------------|-------|
| 1 | Proxy DLL, export forwarding, config, logging | done |
| 2 | Device hooks, OpenVR session, per-eye targets, compositor submit (mono) | done |
| 3 | Camera discovery, head tracking, per-eye view/projection | not started |
| 4 | UI layer split, body-locked floating panel | not started |
| 5 | True per-eye rendering (draw-call duplication) | not started |
| 6 | Zero-copy presenter, comfort and performance polish | not started |

Measured on an RTX 4080 with an Index at 120 Hz, 1480x1644 per eye: capture ~0 ms, upload 2.8 ms,
submit 0.3 ms, with 4.6 ms per frame still spent idle waiting on the compositor. Frame pacing is
locked to the headset.

## Building

Requires Visual Studio 2022 with the C++ workload and CMake. The client is 32-bit, so the DLL
must be too — the `-A Win32` is not optional.

```
git clone --depth 1 --filter=blob:none --sparse https://github.com/ValveSoftware/openvr.git third_party/openvr
cd third_party/openvr && git sparse-checkout set headers lib/win32 bin/win32 && cd ../..

cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release --target deploy
```

`deploy` copies `d3d9.dll`, its PDB and `openvr_api.dll` into the client folder configured by
`WOWVR_CLIENT_DIR` (see the top of `CMakeLists.txt`).

To remove WoWVR, delete `d3d9.dll` from the client folder. The client reverts to stock.

## Configuration

`WoWVR.ini` is created next to the DLL on first run, with every key commented. Keys added by later
builds are written into an existing file automatically, so the INI never silently lags the code.

Two worth knowing about while developing:

- `[General] Enabled=0` — full pass-through. Useful for confirming a problem is ours.
- `[Debug] DumpFrameNumber=N` — writes eye buffer N to `WoWVR_frame.bmp`. This is how the render
  path gets checked without wearing the headset, and it is the fastest way to tell a black frame
  from an upside-down one.

`WoWVR.log` sits next to the DLL and can be read while the game is running.

## Layout

```
src/core/      config, logging, paths, matrix maths, frame dumps
src/proxy/     d3d9 exports, vtable hooking, device hooks
src/vr/        OpenVR session, poses, eye frusta, compositor submit
src/render/    D3D9-side eye targets
src/present/   D3D9 -> D3D11 bridge for the compositor
```

## Notes for anyone picking this up

**The proxy has to pin itself.** WoW resolves `Direct3DCreate9` with
LoadLibrary/GetProcAddress/FreeLibrary and keeps only the interface. Without
`GET_MODULE_HANDLE_EX_FLAG_PIN` the DLL is unloaded about 70 ms into startup, long before the
device exists.

**OpenVR grants scene focus only once you wait on it.** Submitting before the first
`WaitGetPoses` fails with `DoNotHaveFocus` (error 101), so the session primes itself with one wait
during init.

**The compositor has no D3D9 texture type.** Frames currently cross to D3D11 through system
memory, which is why the default `RenderScale` is below 1.0. Phase 6 replaces the innards of
`Presenter` with a shared-surface path; the interface is already shaped for it.

**Vtable slot numbers in `d3d9_slots.h` were generated from the SDK header**, not written from
memory. If they ever look wrong, the check is the order of `STDMETHOD` declarations inside
`DECLARE_INTERFACE_(IDirect3DDevice9, IUnknown)`.
