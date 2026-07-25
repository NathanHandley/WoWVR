#pragma once

#include "core/math3d.h"

#include <cstdint>

namespace wowvr
{
    enum Eye
    {
        EyeLeft = 0,
        EyeRight = 1,
        EyeCount = 2
    };

    // Owns the OpenVR runtime connection. Every failure path here is non-fatal:
    // if there is no headset, or SteamVR will not start, the session stays inactive
    // and the game carries on rendering flat to the desktop.
    class VrSession
    {
    public:
        bool Init();
        void Shutdown();

        bool IsActive() const { return m_active; }

        uint32_t RenderWidth() const { return m_renderWidth; }
        uint32_t RenderHeight() const { return m_renderHeight; }
        float DisplayFrequency() const { return m_displayFrequency; }

        // DXGI adapter the headset is attached to, or -1 if the runtime will not say.
        int PreferredAdapterIndex() const { return m_adapterIndex; }

        // Blocks until the compositor is ready for the next frame, then refreshes poses.
        void WaitForFrame();

        bool HasHeadPose() const { return m_headPoseValid; }

        // True once the headset's proximity sensor says it is actually being worn.
        // Recentring before this happens measures "forward" from a headset lying on a
        // desk, which tilts the whole view for the rest of the session.
        bool UserIsPresent() const;
        const Mat4& HeadToStage() const { return m_headToStage; }
        const Mat4& EyeToHead(int eye) const { return m_eyeToHead[eye]; }

        void EyeTangents(int eye, float& left, float& right, float& top, float& bottom) const;

        // 'texture' is an ID3D11Texture2D. Returns false once and logs on failure,
        // then stays quiet so a broken frame cannot flood the log.
        bool SubmitEye(int eye, void* texture);
        void PostSubmit();

    private:
        bool m_active = false;
        uint32_t m_renderWidth = 0;
        uint32_t m_renderHeight = 0;
        float m_displayFrequency = 90.0f;
        int m_adapterIndex = -1;

        bool m_headPoseValid = false;
        Mat4 m_headToStage;
        Mat4 m_eyeToHead[EyeCount];
        float m_tangents[EyeCount][4] = {};

        // Last compositor error seen, so that a change of state gets logged once
        // rather than every frame, and so recovery is visible rather than silent.
        int m_lastSubmitError = 0;
        unsigned long long m_submitErrorCount = 0;
    };

    VrSession& Vr();
}
