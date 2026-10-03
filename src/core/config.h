#pragma once

#include "core/log.h"

namespace wowvr
{
    struct Config
    {
        // [General]
        bool enabled = true;            // false = behave as a pure pass-through proxy
        LogLevel logLevel = LogLevel::Info;

        // [VR]
        bool vrEnabled = true;          // false = leave the game rendering flat to the desktop
        bool flatDebug = false;         // no HMD required; both eyes side-by-side in the game window

        // Renders the world twice, once per eye. Turning this off falls back to one
        // image shown to both eyes: flat, and noticeably wrong on a headset with
        // canted displays, but a useful comparison if stereo misbehaves.
        bool stereo = true;

        // Drops the bloom/glow composite while stereo is on. It is a full-screen
        // quad drawn with the scene projection, so head tracking swings it across
        // the view as a diagonal band, and it is computed by downsampling the
        // side-by-side target, so its contents are both eyes squashed together.
        // Wrong either way; dropping it costs only the glow effect.
        bool skipPostProcess = true;

        // Drops world-pass draws that use pre-transformed (screen-space) vertices.
        // Those ignore the view and projection and carry absolute pixel coordinates,
        // so duplicating them per eye does not move them: they land over the game's
        // own 1920x1080 rectangle inside the wider stereo target and are merely
        // clipped differently by each eye. Full-screen tints and weather overlays are
        // the usual culprits.
        bool skipScreenSpaceWorldDraws = true;
        // Multiplier on the runtime's recommended per-eye size. Below 1.0 by default
        // because the default (copy) presenter pays a GPU readback proportional to
        // pixel count; the zero-copy 9on12 route removes that cost but is opt-in.
        float renderScale = 0.6f;

        // Hand frames to the compositor through a shared GPU surface instead of a
        // round trip through system memory. Falls back to the copy path by itself
        // if no sharing route works; 0 forces the copy path outright.
        bool zeroCopyPresenter = true;

        // Copy path only: read this frame's pixels back NEXT frame instead of
        // stalling on them now. GetRenderTargetData merely queues the copy; it is
        // the same-frame LockRect that waits for the whole GPU frame to drain.
        // With two staging surfaces per eye the lock takes the previous frame's
        // finished readback, for one frame of extra latency on a path that is a
        // frame behind by nature. Off restores the same-frame lock (A/B via the
        // "pipereadback" command).
        bool pipelinedReadback = true;

        // Copy path only: run the D3D11 upload and the compositor submit on a
        // worker thread instead of the game's render thread. The game thread
        // keeps WaitGetPoses (it paces the game and delivers poses) and hands
        // the worker the locked previous-frame staging pointer; the lock is
        // held until the worker is provably idle next frame, so the two threads
        // never touch the same slot. Live toggle: "presenterthread 0|1".
        bool presenterThread = true;

        // Run the client through Windows' D3D9On12 mapping layer. A plain D3D9
        // device cannot share its allocations at all (share handles and the GL
        // interop both fail), but on 9on12 every D3D9 resource is a D3D12 resource
        // underneath and can be lent to the compositor directly - a true zero-copy
        // present. Off by default because the layer's per-draw overhead measured
        // ~28 fps against ~75 native on this client (1100+ world draws a frame);
        // the perfect hand-off is not worth a third of the frame rate.
        bool useD3D9On12 = false;
        float ipdOverride = 0.0f;       // metres; 0 = whatever the headset reports
        float worldScale = 1.0f;        // >1 makes the world feel larger

        // Game units per real-world metre, used to turn head movement into camera
        // movement. WoW's unit is roughly a yard, so 1 m is about 1.09 units. Raise it
        // if leaning moves the view too little, lower it if it moves too much.
        float unitsPerMetre = 1.0936f;

        // Per-shader search for a combined world-view-projection. Registers are aliased
        // across shaders - c0 is the water's transform and something else entirely
        // elsewhere - so the register has to be resolved per shader rather than assumed.
        // The scan costs a constant-file walk per draw, but only until each shader is
        // either resolved or given up on, so it is a startup cost rather than a
        // per-frame one.
        bool perShaderCombined = true;

        // Rewrite combined world-view-projection transforms at all. Off is a bisect: if
        // the terrain shader derives its shadow lookup from the same matrix that
        // positions the vertex, then rewriting it necessarily corrupts the shadow.
        bool patchCombined = true;

        // Rebuild constants derived from inverse(sceneProjection) against the eye
        // projection. Needed for the client's shadow lookup.
        bool patchInverseDerived = false;

        // Write every vertex shader's bytecode to disk on creation, for offline
        // disassembly. Off by default: it is a few hundred small files.
        bool dumpShaders = false;

        // [Camera]
        bool headTracking = true;       // HMD orientation drives the in-game view
        float cullFovScale = 1.6f;

        // Multiplies the field of view the client CULLS against, inside the camera object.
        //
        // Distinct from CullFovScale, which multiplies the global setting at 0x00ABFC38 -
        // that one is inert, because the client copies the setting into the active camera
        // once a frame and reads the copy. This is the copy.
        //
        // Wanted because the camera is aimed at the head at the end of one frame and culled
        // against during the next: without margin a fast head turn outruns the frustum. It
        // also covers head roll, which is not applied to the game camera at all. 1.0
        // disables it; the client's rendering degrades at genuinely extreme angles, so the
        // result is capped at 2.6 rad regardless.
        //
        // Set to reach the client's own ceiling, because vertical coverage is the binding
        // constraint and it is only just enough.
        //
        // Measured: the client builds a vertical field of view of exactly 0.6 * the value in
        // the camera, and clamps it at 107.4 degrees. The headset needs 109.4. So there is
        // no headroom to trade away - anything less and the ground is culled from under you
        // as soon as the view pitches up, which at the previous 1.25 (67.5 degrees vertical)
        // it visibly was.
        //
        // 1.5708 * 2.05 = 3.22, just past where the clamp takes over. The horizontal that
        // falls out of this is 152 degrees at a 2.96 aspect, far more than the 114 an eye
        // needs, and all of that surplus is geometry drawn and never displayed. A squarer
        // window is the way to give that back: at 1920x1440 the same vertical comes with
        // 122 degrees horizontal instead.
        float cullWidenScale = 2.05f;

        // Widens the client's shadow cascades so their coverage reaches past what the
        // headset can see. WoW sizes them for its own ~59 degree view - the near cascade
        // spans about 13 yards - so in VR the edge of the coverage is visible as a
        // dark-edged region that slides with the player.
        //
        // BOTH sides must be scaled by the same factor or casters are written at one
        // scale and sampled at another: the light's projection while the shadow map is
        // rendered, AND the terrain shader's cascade transforms at c224/c225, c227/c228,
        // c230/c231. Scaling only one is worse than doing nothing.
        //
        // 1.0 disables it. Widening ALONE is not enough: growing the world area each
        // shadow texel covers outruns the client's baked-in depth bias and everything
        // self-shadows, so ShadowDepthBias has to grow with it.
        float shadowCoverageScale = 1.0f;

        // Bias added to each cascade's compare depth, needed once a cascade is widened.
        // The compare depth is dot(worldPos, c226) for cascade 0, so its translation
        // component is exactly where the bias belongs.
        float shadowDepthBias = 0.0f;

        // Searches process memory for WoW's camera. Off by default: each pass walks
        // hundreds of megabytes with the render thread blocked, which the player feels
        // as the headset briefly dropping out, and it has not succeeded yet.
        bool scanForCamera = false;

        // Read-only watch on the structure that holds the field of view setting, and on
        // the camera position recovered from the matrices on the wire. Reports which of
        // the structure's floats change as the player moves and turns, which is how the
        // per-frame camera state is told apart from the settings around it.
        bool watchCameraStruct = false;

        // Locates the game's camera during play so its yaw can be aimed at the headset.
        // Costs one sweep of memory shortly after entering the world, then only a cheap
        // re-check. Nothing is written unless AimCameraAtHead is also on.
        bool autoLocateCamera = true;

        // Turns the game's own camera to follow the head, so it culls and streams for
        // where you are looking rather than where the character faces. This is the only
        // setting that writes to the client's memory; it does nothing until the camera
        // has been located, and it stops the moment that object stops looking right.
        //
        // Off by default: the client bakes its distant pass (sky, backdrop terrain, far
        // WMO canopy) against a heading that adopts the aim over a second or two, so
        // continuous aiming makes the sky drag with the head during real worn motion.
        // The widened static frustum covers normal head movement without any writes.
        bool aimCameraAtHead = false;

        // Rotates the six planes the client culls against so they follow the head, while
        // leaving its camera exactly where the character put it.
        //
        // This is the third answer to head-driven culling and the first that does not go
        // through the camera. Aiming the camera works for culling and drags the sky,
        // because the distant pass adopts the aim over a second or two; a deadzone hides
        // the drag behind a visible pop; drawing everything costs 114 fps to 68. Rotating
        // the culling volume alone has none of those: the camera's heading never moves so
        // the distant pass has nothing to lag behind, the viewpoint never translates so
        // there is nothing to correct, and the volume is the same size pointed elsewhere
        // so the draw count is unchanged.
        //
        // Independent of AimCameraAtHead, and meant to replace it. Both on would rotate
        // the frustum of an already-aimed camera, which double-counts the head.
        bool headDrivenCullFrustum = true;

        // What shape a culling volume has to be before the head is allowed to turn it.
        //
        // The client builds two kinds through the same function: the camera's whole view,
        // and views clipped to an opening you are looking through. Both are in world space
        // and both are apexed at the camera, so neither the space they are in nor where
        // they were built from tells them apart - only their shape does. Turning a clipped
        // one swings it off the opening and takes the building beyond the doorway with it,
        // which is a WMO disappearing while you look straight at it.
        //
        // Measured in the Stormwind gate's archway: the camera's own view is 70.2 degrees
        // wide with its corners averaging 0.0 degrees off the forward axis; the two
        // clipped to the arch are 43.5 and 31.6 wide, averaging 26.1 and 24.2 off it. The
        // average is the real test and it is exact - a perspective frustum is symmetric
        // about the axis it was built on. The width covers the one case symmetry does not,
        // an opening dead ahead, and 40 clears the narrowest the camera's own view ever
        // gets, which is 46 with CullWidenScale at 1.0.
        //
        // 0 and 180 turn everything, which is the fault these exist to prevent.
        float cullRotateMinSpanDegrees = 40.0f;
        float cullRotateMaxOffAxisDegrees = 5.0f;

        // Draws terrain all the way around the camera while the head drives culling.
        //
        // The rotated clip volumes steer doodads, WMOs and game objects, but ADT
        // terrain never tests against those planes: its directional gate is a view-cone
        // angle test against cos(cullFov/2), recomputed each frame at 0x00CD877C from
        // the character's heading. With the cull FoV at the client's ceiling that cone
        // covers about +/-90 degrees, so a head turned further stands over bare void
        // while the buildings and trees on it render - the Goldshire bug. This writes
        // -1 over the cosine so the angle test always passes; horizon occlusion, the
        // 50-unit rule and the chunk index box are untouched, and the client's per-frame
        // recompute means turning it off restores stock terrain culling by itself.
        bool cullTerrainAllAround = true;

        // Rotates the master frustum corner array (0x00CDB108) to the head, once per
        // client refresh. This is what makes TERRAIN cull where the head looks: the
        // per-volume rotation only ever steers copies derived from this array, and
        // terrain reads the original. Found by elimination - with every copy rotated
        // and every bit-gated terrain test disabled, the ground still vanished beyond
        // the widen margin while the buildings and doodads on it followed the head.
        bool cullRotateMasterCorners = true;

        // Which of the master corner array's fixed-address READ sites are patched to
        // consume the UNROTATED shadow copy instead of the live, head-rotated array
        // (bit i = site i; the sites are listed in cull_frustum.h). This is the
        // portal-vs-terrain split of the master's consumers: the client derives its
        // portal-clipped interior volumes by combining the portal's screen rectangle
        // - projected with the real, unturned camera - with the master corners, so a
        // rotated master swings every volume past the first doorway off the room it
        // was cut to. Measured in the Lion's Pride Inn: the room beyond one doorway
        // blanked at 17 degrees of head yaw with the master rotated, and returned
        // with it left alone. Terrain has no screen-space half and needs the rotated
        // set, so the read sites are separated rather than choosing one behaviour
        // for both. Bisected live with cullfeed/cullfeedmask.
        //
        // 4 = site 2 (the push at 0x007AC466) alone, which the bisection settled: at
        // a forced 20-degree turn, shadow-feeding site 2 restored the room beyond
        // the doorway to pixel-parity with the unrotated control while sites 0, 1
        // and 3 each changed nothing; the interior then held at forced 45, 60 and
        // 90; and the forced-150 outdoor terrain void was identical with site 2 on
        // either feed, so it is not a terrain reader.
        unsigned cullMasterShadowFeeds = 4;

        // Draws every group of a WMO the walk already accepted, by answering "inside"
        // for the map-object family of the shared bounds test.
        //
        // Off by default because it was measured NOT to fix the fault it was built
        // for: WMO walls and floors still vanish once the culling volume is turned
        // past roughly 45 degrees, with this bypass covering the full family - so the
        // failing test is somewhere else entirely - while the bypass itself inflated
        // shadow caster draws badly (973k per report period against 2.4k stock).
        // Kept because the detour, the family range command (cullwmorange) and the
        // toggle (cullwmoall) are the instruments the next investigation needs.
        bool wmoGroupsAlwaysVisible = false;

        // [Sound]
        // Puts FMOD's listener on the headset instead of the client's camera: the ears
        // turn with the head, so a sound to the left of the headset is heard on the left,
        // and they walk with it, so stepping towards something in the room brings it
        // closer. The client's own placement rule - its Sound_ListenerAtCharacter,
        // Sound_ListenerBackDist and Sound_ListenerUpDist CVars - still chooses where the
        // listener starts from; this is a correction applied on top of it.
        bool headDrivenSoundListener = true;

        // The half of that which needs a scale to be believed. Off keeps the rotation and
        // drops the walk, which is the setting to reach for if the room and the world
        // disagree about how far a step is.
        bool soundListenerFollowsHead = true;

        // Start the correction from the CAMERA rather than from wherever the client put
        // the listener. Its own answer is the character - Sound_ListenerAtCharacter
        // defaults on, and that branch takes the position from the player object while
        // still taking the orientation from the camera - so distance to a sound never
        // responded to the view at all: zooming a third-person camera out over a fire left
        // the fire exactly as loud. In a headset the viewpoint is the head, so the ears
        // belong at the camera, which is what every other part of this project already
        // treats as the eye origin.
        bool soundListenerAtCamera = true;

        // Turns off the client's third-person camera collision, by removing one conditional
        // jump in its own code. Aiming the camera at the head sweeps it through terrain, and
        // the client's answer is to pull it in - measured going from 16 yards to 0.79 in a
        // single head pitch, which moves the viewpoint fifteen yards and cannot be corrected
        // for afterwards because the camera really is somewhere else.
        //
        // On by default, and only while VR is actually driving the camera; a flat session in
        // the same client gets the client's own behaviour back.
        bool disableCameraCollision = true;

        // Last-resort camera identification: alters candidate addresses to see which
        // one the game rebuilds its projection from. Conclusive, but it writes to
        // memory that may belong to something else, and it has crashed the client.
        bool probeCameraByWriting = false;      // widen WoW's culling frustum so edges do not pop

        // [Panel]
        // The interface is a curved sheet - a section of an upright cylinder around the
        // head - that stays where it was put. Ctrl+Alt+F8 puts it straight ahead again
        // (yaw only, centred at eye height); Ctrl+Alt+F11 recentres it with the view.
        float panelDistance = 1.6f;     // metres: the cylinder's radius

        // How many of the interface's pixels fit in one degree of arc. This fixes the
        // size elements appear at, so a higher game resolution makes the panel bigger
        // - more room between elements - instead of making everything smaller. 27.8 is
        // what the old flat 2.2 m panel at 1.6 m gave a 1920-wide interface.
        float panelPixelsPerDegree = 27.8f;

        // Upper bound on how far round the panel may wrap. Past it the whole panel
        // shrinks, aspect kept, rather than closing into a ring.
        float panelMaxArcDegrees = 150.0f;

        // Aim the mouse into the world along the line from the HEAD through the pointer
        // on the panel, and place nameplates, floating combat text and chat bubbles
        // where that same line from the head meets the panel. Without it the client
        // picks and places along its own flat camera, which the headset never shows
        // and which is widened for culling, so neither lines up with anything.
        bool panelWorldPointing = true;

        float panelOpacity = 1.0f;

        // The pointer is drawn at the size the game uses, which is small seen through a
        // headset. This multiplies it without touching where it actually points.
        float cursorScale = 1.5f;

        // Keeps the mouse inside the game window while the game has focus. Wearing the
        // headset you cannot see the pointer leave, and once it has, the panel stops
        // drawing it and clicks land in whatever is behind. Released on focus loss, so
        // alt-tab still works.
        bool confineCursor = false;

        // Treat the interface panel as premultiplied alpha: blend the alpha channel on
        // its own factors while the interface renders, and composite the panel without
        // multiplying by alpha a second time. Off reverts to straight-alpha compositing,
        // which darkens anything translucent and turns additive highlights and glows
        // into black patches.
        bool premultipliedUi = true;

        // [Mirror]
        bool desktopMirror = true;      // keep drawing something in the game window

        // [Debug]
        // Writes the eye buffer of this frame number out as WoWVR_frame.bmp so the
        // render path can be checked without putting the headset on. 0 disables it.
        int dumpFrameNumber = 0;

        // Logs a detailed report of everything the game does to the device during
        // this frame: transforms, constant uploads, shaders, draws, render targets.
        // 0 disables it.
        int frameReportNumber = 0;

        // Writes the stereo target out every N world draws of the dumped frame, so the
        // exact draw that introduces an artefact can be found by looking rather than by
        // guessing at what it might be. 0 disables it. Expensive: that frame takes
        // seconds and each image is ~20 MB.
        int dumpEveryNWorldDraws = 0;

        // Bisect switch: hands the game's own projection back untouched instead of
        // substituting the eye frustum. Everything else - stereo duplication, the UI
        // panel, the render targets - stays exactly as it is. If an artefact survives
        // this, the projection substitution is not what causes it.
        bool useGameProjection = false;

        // Independent bisects for the two substitution paths, so they can be told apart.
        bool patchConstants = true;
        bool patchFixedFunction = true;

        // Substitute the game's own projection back, unchanged, through the same path.
        // Separates "our substitution machinery is broken" from "the wider field of view
        // is what the client cannot cope with".
        bool eyeProjectionPassThrough = false;

        // Draw only the left eye, still into the side-by-side target. Separates "drawing
        // everything twice" from "rendering into our own target" as the cause of a fault
        // that only appears in stereo.
        bool singleEyeOnly = false;

        // Build each eye's frustum symmetrically instead of off-centre. The Index's eyes
        // are canted, so the projection centre sits ~9% off middle; anything that looks
        // the shadow up in screen space would be displaced by exactly that much.
        bool symmetricEyeProjection = false;

        // Leave the client's own viewport alone during the eye passes. The image will be
        // wrong (both eyes overlap), but it isolates whether anything in the shadow path
        // depends on the viewport we substitute.
        bool keepGameViewport = false;

        // Draw a single eye across the WHOLE side-by-side target, so the viewport and
        // the render target coincide. If anything derives a screen-space lookup from the
        // viewport while sampling the full target, this is the configuration where the
        // two agree and the fault should disappear.
        bool fullTargetSingleEye = false;

        // Log what the post-process skip rule discards.
        bool logSkippedDraws = false;

        // Logs what the pointer's shape was chosen from, whenever that changes:
        // focus, the handle the client last set, and the global cursor.
        bool logCursorDecisions = false;

        // Log the shadow cascade constants c224..c235 as uploaded.
        bool logShadowConstants = false;

        // Logs full state for world draws in this range of the dumped frame, so an
        // artefact narrowed down by the sequence dump can be identified rather than
        // guessed at. Inclusive; 0/0 disables.
        int logWorldDrawFrom = 0;
        int logWorldDrawTo = 0;

        // Probe for the camera hunt. When not 1.0, the horizontal scale of the
        // matrix at vertex shader constant c2 is multiplied by this on its way to the
        // device. If c2 really is the shared projection, the 3D scene visibly changes
        // shape and the 2D UI does not. Anything but 1.0 makes the game look wrong on
        // purpose; it is a diagnostic, not a setting.
        float projectionProbeXScale = 1.0f;
    };

    // Reads WoWVR.ini from next to the DLL, writing a commented default file first
    // if none exists. Safe to call more than once (used by the reload hotkey).
    void LoadConfig();

    const Config& Cfg();
}
