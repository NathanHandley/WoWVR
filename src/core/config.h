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
        // because the current presenter moves every frame through system memory, and
        // SteamVR's recommendation for an Index is already heavily supersampled.
        float renderScale = 0.6f;
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

        // Last-resort camera identification: alters candidate addresses to see which
        // one the game rebuilds its projection from. Conclusive, but it writes to
        // memory that may belong to something else, and it has crashed the client.
        bool probeCameraByWriting = false;      // widen WoW's culling frustum so edges do not pop

        // [Panel]
        float panelDistance = 1.6f;     // metres in front of the body
        float panelWidth = 2.2f;        // metres
        float panelDeadzoneDegrees = 20.0f;
        float panelFollowSpeed = 4.0f;  // higher = panel catches up to your body faster
        float panelOpacity = 1.0f;

        // The pointer is drawn at the size the game uses, which is small seen through a
        // headset. This multiplies it without touching where it actually points.
        float cursorScale = 1.5f;

        // Keeps the mouse inside the game window while the game has focus. Wearing the
        // headset you cannot see the pointer leave, and once it has, the panel stops
        // drawing it and clicks land in whatever is behind. Released on focus loss, so
        // alt-tab still works.
        bool confineCursor = false;

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
