#pragma once

namespace wowvr
{
    // All of these need Ctrl+Alt held, so they cannot collide with WoW's own
    // bindings (a bare F9 opens a bag).
    enum class Hotkey
    {
        FrameReport,   // Ctrl+Alt+F9  - log a full frame report
        FrameDump,     // Ctrl+Alt+F10 - write the current eye buffers to BMPs
        Recenter,      // Ctrl+Alt+F11 - recentre the view and the UI panel
        ReloadConfig,  // Ctrl+Alt+F12 - re-read WoWVR.ini
        RecenterUi,    // Ctrl+Alt+F8  - bring the UI panel in front of the head again
        Count
    };

    // True exactly once per physical key press. Must be polled once per frame from
    // the render thread; the edge detection is per-process, not per-caller.
    bool HotkeyPressed(Hotkey key);
}
