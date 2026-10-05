# WoWVR

SteamVR support for a 3.3.5a (12340) World of Warcraft client, aimed at the Valve Index.

The world is rendered stereoscopically around you and the headset drives the view. Movement,
targeting and everything else stay on mouse and keyboard — there is no motion-controller input.
The game UI is lifted out of the eye buffer and hung in space on a curved sheet that stays where
you put it. The mouse points into the world from your head through that sheet, and nameplates sit
where your head sees the creature through it.

It works as a proxy `d3d9.dll` that the client loads out of its own folder. Nothing in the client
is patched on disk.

## Commands

The mouse is kept inside the game window while it has focus (`[Panel] ConfineCursor=1`), so stray
clicks can't land on the desktop; alt-tab releases it.

ALT + CTRL + F1 shows or hides a list of all the WoWVR commands (a reminder of this shows in the
corner for the first 10 seconds; `[Panel] LaunchHintSeconds` changes that, 0 turns it off)

ALT + CTRL + F11 "recenters" the view (and puts the interface back in front of you)

In the world the interface sits on a canvas twice as wide and tall as the screen (`[Panel] CanvasScale`,
1 = off): everything stays where and as big as it was, with room around it for the mouse, dragged
frames, lifebars and chat bubbles.

Lifebars (the V key) are drawn over each unit in the world at its own distance rather than on the
interface (`[Panel] Nameplates3D`; while it's on, `CanvasScale` is at least 1.25). They can't be clicked to
target.

ALT + CTRL + PAGE UP / PAGE DOWN pushes the interface farther away / pulls it closer (hold to keep
moving; the distance is saved to WoWVR.ini)

ALT + CTRL + F8 recenters only the interface: it turns to face where you are looking (left/right
only) with its middle at eye height

## Status

Both first person and third person perspectives are supported.  No motion controls or comfort controls yet, but that's next

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

`WoWVR.ini` is created next to the DLL on first run with lots of config options (that have comments)

## Credits

Claude (Opus 5 and Fable) actually wrote most of the code, so releasing this under MIT