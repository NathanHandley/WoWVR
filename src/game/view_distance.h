#pragma once

namespace wowvr
{
    // How far the client draws the world.
    //
    // The client recomputes its draw distance every frame in its world update
    // (0x00781430): 0x00780770 turns the farclip setting into a far distance, clamped
    // to 791.67 yards on the old continents (1583.33 elsewhere, memory permitting),
    // and stores it at 0x00CD7748 - with a copy at 0x00CD7744 that the streaming code
    // compares against to decide when to reload. Everything downstream reads it: the
    // camera's far plane (copied in by 0x00607B00), culling, the projection, the
    // low-detail backdrop. That clamp is why the old world ended in a flat cut-off
    // square to the view, nearer straight ahead than off to the side.
    //
    // The detour sits on the instruction right after the store (0x0078144F), with the
    // freshly computed far still on the FPU stack: it multiplies it, rewrites
    // 0x00CD7748, and lets the client's own next instruction store the same scaled
    // value into 0x00CD7744 - so the pair stays consistent and nothing reloads every
    // frame. Writing the camera's far field instead does nothing: the camera update
    // overwrites it from 0x00CD7748 before anything reads it.
    class ViewDistance
    {
    public:
        bool Install();

        // 1.0 leaves the client's distance alone. Takes effect on the client's next
        // world update.
        void SetScale(float scale);

        // The last distance the client computed, and what it was turned into.
        float ClientDistance() const;
        float DrawDistance() const;
    };

    ViewDistance& DrawRange();
}
