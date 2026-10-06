#pragma once

namespace wowvr
{
    // Comfort vignette: darkens the edges of the view while the view moves in a way the
    // player's body does not - the character walking, turning or being carried, the
    // camera dragged with the mouse or zoomed - and never for the head's own movement.
    //
    // What counts as "not the head" is read from the client, not inferred from the image:
    //   - the active player's position and facing (ClntObjMgrGetActivePlayerObj,
    //     0x004038F0; position at +0x798, facing at +0x7A8): movement keys, turning,
    //     mounts, lifts, knockbacks;
    //   - the camera's own direction with WoWVR's head aim taken back out (the free-look
    //     fields WoWVR writes from the head are subtracted from the camera's measured
    //     facing): mouse drags;
    //   - the camera's orbit distance: zooming.
    // A jump too big for one frame (a teleport, a loading screen) is ignored rather than
    // treated as motion.
    class ComfortVignette
    {
    public:
        enum Size { Small = 0, Medium = 1, Large = 2 };

        // Once a frame, from the render thread (the client's object manager is read
        // through the main thread's TLS). 'seconds' is the frame time.
        void Update(float seconds, bool enabled);

        // 0 = no vignette, 1 = fully drawn. Already eased in and out.
        float Amount() const { return m_amount; }

        // Shows the vignette fully for a moment, so a size change or switching it on can
        // be seen without having to move.
        void Preview(float seconds) { m_previewSeconds = seconds; }

        // For the log: how much artificial motion was seen recently, and why.
        void LogStatus();

    private:
        bool m_havePrevious = false;
        float m_previousPosition[3] = {};
        float m_previousFacing = 0.0f;
        float m_previousCameraYaw = 0.0f;
        float m_previousCameraPitch = 0.0f;
        float m_previousOrbit = 0.0f;
        float m_holdSeconds = 0.0f;
        float m_previewSeconds = 0.0f;
        float m_amount = 0.0f;
        unsigned long long m_framesMoving = 0;
        unsigned long long m_framesSeen = 0;
        unsigned m_reasons = 0;
    };

    ComfortVignette& Vignette();
}
