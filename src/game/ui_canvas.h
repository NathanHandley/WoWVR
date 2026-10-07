#pragma once

namespace wowvr
{
    // A bigger interface canvas in the headset without making anything on it bigger.
    //
    // The game's screen is capped by the desktop (the window cannot be taller than the
    // monitor), so the canvas cannot grow by giving the client more pixels. Instead the
    // panel stretches the same screen over CanvasScale times the angle in each
    // direction, and the client's interface is drawn at 1/CanvasScale scale to match:
    //
    //   - UIParent (action bars, minimap, unit frames, chat...) is scaled down by the
    //     same factor and confined to the middle of the screen at its old size in its
    //     own units. In the headset every element is exactly as big and exactly where
    //     it was; the rest of the screen - where the mouse can go, where frames can be
    //     dragged - is the new space around it.
    //   - WorldFrame is scaled down too, which is what lifebars (nameplates) and chat
    //     bubbles hang off: they stay their old size, and can now appear anywhere over
    //     the larger panel.
    //
    // Done with a few lines of the client's own Lua, run from the render thread through
    // FrameScript_Execute (0x00819210) - only in the world (UIParent does not exist on
    // the login screens), only out of combat, and re-applied whenever the client resets
    // the interface scale (login, /reload, a UI scale change). The panel switches to
    // the stretched geometry on exactly the frames the layout is applied, so the
    // login screens keep the ordinary panel.
    //
    // The price is sharpness, not speed: the interface is drawn at half the pixels per
    // degree it had - about the headset's own density instead of twice it.
    class UiCanvas
    {
    public:
        // Verifies the client's code at the two entry points before ever calling them.
        bool Install();

        // Once a frame, from the render thread. 'wanted' is CanvasScale when the
        // feature should be on (VR running), 1 otherwise.
        void Update(float wanted);

        // How much the panel is stretched right now: 1, or the applied CanvasScale.
        float PanelScale() const;

        // Runs a Lua snippet through the client's own entry point, for the "lua" debug
        // command. Only in the world, from the render thread; false if it could not run.
        bool RunDebugScript(const char* code);
    };

    UiCanvas& Canvas();
}
