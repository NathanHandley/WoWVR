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

        // [Camera]
        bool headTracking = true;       // HMD orientation drives the in-game view
        float cullFovScale = 1.6f;

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
