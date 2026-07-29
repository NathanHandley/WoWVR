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
| 3 | Camera discovery, head tracking, per-eye view/projection | done |
| 4 | UI layer split, body-locked floating panel | done |
| 5 | True per-eye rendering (draw-call duplication) | done |
| 6 | Zero-copy presenter, comfort and performance polish | partial |

Phase 6's zero-copy path works only through Windows' D3D9On12 layer, whose per-draw overhead
costs more than the readback it saves on this client, so it is off by default. See
`UseD3D9On12` in the INI.

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
src/stereo/    projection substitution, side-by-side stereo target
src/ui/        the body-locked interface panel
src/game/      the client's own camera, culling volumes and sound listener
src/diag/      per-frame reports
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

**Culling follows the head, and it does it without touching the camera.** The client decides
what to draw against a frustum built from the character's heading, about 54° wide. An eye sees
110° and can turn 180°, and anything the client did not consider is not dark or low-detail, it
is absent. Three earlier answers all went through the camera object and all failed on it:
aiming it continuously drags the sky, because the client's distant pass adopts the aim over a
second or two; aiming it on a deadzone trades that drag for a visible pop; and disabling culling
outright costs 114 fps to 68.

`src/game/cull_frustum.cpp` instead detours the client's own plane builder and turns the culling
volume itself, so the camera keeps the character's heading and nothing downstream of it moves.
Two things about it are not guessable and are the whole reason it works — both are written up at
length in the source, and in `CLIENT_INTERNALS.md` §8.7–8.12 if you have those docs:

- **Turn the eight corners, not the six planes.** The client derives further volumes from a
  volume's corners, so planes rewritten after the fact are invisible to that derivation and map
  data goes on being culled against the character's heading no matter what the planes say.
- **Only turn a volume that is still in world space.** Most rebuilds are not — the world view
  alone is carried into another frame 36 times a frame. The test needs no addresses: a frustum's
  four side planes meet at its apex, so the camera reads ≈0 from all four if and only if the
  volume is still in the camera's frame.
- **Only turn a volume that is the camera's whole view.** The client also builds volumes clipped
  to whatever opening you are looking through, and those are in world space and apexed at the
  camera too, so the previous test passes them. Turning one swings it off its opening, and what
  vanishes is not at the edge of vision — it is the building on the far side of the doorway,
  dead ahead. Shape is what separates them, and the measurement is not close: standing in
  Stormwind's gate the camera's own view is 70.2° wide with its corners averaging 0.0° off the
  forward axis, while the two clipped to the archway are 43.5° and 31.6° wide averaging 26.1°
  and 24.2° off it. The average is the real test and it is exact rather than empirical — a
  perspective frustum is symmetric about the axis it was built on, so the mean of its eight
  corners *is* that axis. `CullRotateMinSpanDegrees` and `CullRotateMaxOffAxisDegrees`, with
  `cullrotshape 0 180` to put the fault back for an A/B.

Off by default it is not — `HeadDrivenCullFrustum` in the INI, with `cullroton` / `cullrotoff`
through the command channel for an A/B without a restart.

**The ears follow the head too, and by the same shape of hook.** The client places FMOD's
listener from its camera — optionally pulled back and lifted, or parked on the character,
depending on three `Sound_Listener*` CVars — and points it along the camera's forward axis.
Worn, that leaves the ears on the character while the eyes have followed the head since
phase 3: turning your head does not move the stereo image, and stepping towards something in
the room does not bring it closer.

`src/game/sound_listener.cpp` detours `0x004C5B20`, the four-argument wrapper every one of
the client's placement branches funnels through on its way to FMOD's
`Set3DListenerAttributes`, and turns the orientation it was handed by the head's rotation
and walks the position by the head's displacement. Both are the same two quantities the eye
projection already applies, so the ears and the eyes cannot disagree.

The position also has to be *rebased*. `Sound_ListenerAtCharacter` defaults on, and that
branch takes the position from the player object while still taking the orientation from the
camera — measured, the listener sat 25.2 yards from the camera in third person, which is why
direction responded to the head and distance responded to nothing at all. The ears are put
at the camera instead, that being what the rest of this project already treats as the eye
origin. `HeadDrivenListener`, `ListenerFollowsHeadPosition` and `ListenerAtCamera` in the
INI; `sndon` / `sndoff` / `sndcam 0` / `sndstate` through the command channel.
`CLIENT_INTERNALS.md` §9 has the chain.

**There is a command channel for experiments.** A single line written to `WoWVR_cmd.txt` next to
the DLL is read within a few frames and the file is then deleted, which is the acknowledgement.
It exists because SteamVR takes focus while it starts and silently swallows hotkeys, which made
scripted runs impossible to drive any other way. `cullrotstate` is a good first one to try: it
prints every culling volume the client rebuilt, which call site built it, and how far the camera
sits from its side planes.

## Credits

Claude (Opus 5 and Fable) actually wrote most of the code, so releasing this under MIT