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

        // [Camera]
        bool headTracking = true;       // HMD orientation drives the in-game view
        float cullFovScale = 1.6f;      // widen WoW's culling frustum so edges do not pop

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
