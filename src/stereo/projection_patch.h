#pragma once

#include "core/math3d.h"

namespace wowvr
{
    // Substitutes WoW's camera projection with a VR one.
    //
    // The client uploads its projection as a 4-register vertex shader constant, and
    // whatever feeds it is already in view space (the matrix carries no rotation or
    // translation of its own). That means head rotation, the per-eye offset and the
    // headset's own projection all compose into that single constant:
    //
    //     replacement = R_head * T_eye * P_vr
    //
    // No per-draw work and no shader analysis required.
    //
    // The catch is that the same register is reused by other passes in the same
    // frame - the shadow map at 2048x2048 has its own projection - so the block has
    // to be identified as the main camera before it is touched.
    class ProjectionPatch
    {
    public:
        // Aspect ratio of the game's back buffer, used to tell the scene camera apart
        // from the square shadow-map pass.
        void SetSceneAspect(float aspect);

        // Treats the next head orientation as looking straight ahead.
        void Recenter();

        // Refreshes the head rotation from the current HMD pose. Called once a frame.
        void UpdateFromHeadPose(const Mat4& headToStage);

        // Examines a 4-register upload. When it is the scene camera, fills both eye
        // replacements (16 floats each, in the upload's own layout) and returns true.
        // Returns false to leave the upload alone.
        bool TryPatch(const float* uploaded, float* outLeft, float* outRight);

        // Head yaw relative to the recentred origin, in radians, in the game's
        // left-handed convention. Drives the body-locked panel.
        float HeadYaw() const { return m_headYaw; }

        // The scene camera's own parameters, as decoded from the matrix the game
        // uploads. These are what the camera in memory must also be holding, which is
        // how it gets found without trusting a published offset.
        bool HasSceneProjection() const { return m_patched > 0; }
        float SceneNear() const { return m_sceneNear; }
        float SceneFar() const { return m_sceneFar; }
        float SceneAspect() const { return m_sceneAspect; }
        float SceneVerticalScale() const { return m_sceneVerticalScale; }

        // The same correction applied to the world: rotation into the current view
        // frame, and displacement since recentring, in metres. Anything that wants to
        // sit still in space has to use these, not just the yaw.
        const Mat4& HeadRotation() const { return m_headRotation; }
        const Vec3& HeadOffsetMetres() const { return m_headOffset; }

        // Diagnostics for the log.
        unsigned long long PatchedCount() const { return m_patched; }
        unsigned long long RejectedCount() const { return m_rejected; }
        void LogLastDecision() const;

    private:
        struct Decoded
        {
            Mat4 projection;      // row-vector convention, whatever the upload used
            bool wasTransposed = false;
            float horizontalScale = 0.0f;
            float verticalScale = 0.0f;
            float nearPlane = 0.0f;
            float farPlane = 0.0f;
        };

        bool Decode(const float* uploaded, Decoded& out) const;

        float m_sceneAspect = 16.0f / 9.0f;
        Mat4 m_headRotation = Mat4Identity();
        Mat4 m_neutralInverse = Mat4Identity();
        Vec3 m_neutralPosition;
        Vec3 m_headOffset;              // metres, relative to the recentred origin
        float m_headYaw = 0.0f;
        bool m_recenterRequested = true;

        // Perspective matrices that were recognised but not treated as the scene
        // camera. If the sky or any other pass has a projection of its own, it
        // shows up here - and being left unpatched while the world is patched is
        // exactly how geometry ends up in the wrong place at the wrong depth.
        struct RejectedProjection
        {
            float aspect = 0.0f;
            float nearPlane = 0.0f;
            float farPlane = 0.0f;
            unsigned long long count = 0;
        };
        static constexpr int kMaxRejected = 8;
        RejectedProjection m_rejectedProjections[kMaxRejected];
        int m_rejectedProjectionCount = 0;

        unsigned long long m_patched = 0;
        unsigned long long m_rejected = 0;
        float m_sceneNear = 0.0f;
        float m_sceneFar = 0.0f;
        float m_sceneVerticalScale = 0.0f;
        float m_lastAspect = 0.0f;
        float m_lastNear = 0.0f;
        float m_lastFar = 0.0f;
    };

    ProjectionPatch& Projection();
}
