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

        // How much of the head's yaw the game's own camera has already been turned by.
        //
        // Once the game's camera follows the head, the world arrives already rotated -
        // the client bakes its view transform into the vertices on the CPU. Adding the
        // same rotation again here would turn the world twice as far as the head moved,
        // which is both wrong and unpleasant to wear. So this is subtracted back out, and
        // what remains for the projection to apply is the part the game has not done:
        // pitch, roll, and any yaw still catching up.
        void SetGameCameraYaw(float radians) { Bump(m_gameCameraYaw, radians); }

        // The distance the game's camera orbits the character at.
        //
        // Undoing the rotation above is not enough on its own, because the camera is a
        // third-person ORBIT camera: turning its yaw does not pivot it in place, it swings
        // it around the character on a circle of this radius. Cancel only the rotation and
        // the leftover translation reads as the whole view sliding sideways with no change
        // of facing. Zero disables the correction, which is also correct in first person.
        void SetGameCameraOrbitRadius(float yards) { Bump(m_gameCameraOrbitRadius, yards); }

        // The measured view-space displacement aiming the camera caused, in yards.
        void SetOrbitDisplacement(const Vec3& yards)
        {
            Bump(m_orbitDisplacement.x, yards.x);
            Bump(m_orbitDisplacement.y, yards.y);
            Bump(m_orbitDisplacement.z, yards.z);
        }
        void SetGameCameraPitch(float radians) { Bump(m_gameCameraPitch, radians); }

        // A synthetic head pitch, for the same reason as the synthetic yaw: looking up and
        // down has to be testable with the headset lying still on a desk.
        void SetFakeHeadPitch(float radians) { m_fakeHeadPitch = radians; }

        // Stands in for leaning or stepping, in metres, so positional tracking can be
        // exercised with the headset sitting still on a desk.
        // Diagnostic: the order the game camera's two rotations are unwound in. Only has
        // any effect when yaw and pitch are both non-zero, which is exactly the case that
        // went untested and shipped a residual roll.
        void SetUnwindPitchFirst(bool on) { m_unwindPitchFirst = on; ++m_generation; }

        // +1 or -1 on the third-person orbit correction, so the direction can be measured
        // rather than argued about.
        void SetOrbitSign(float sign) { Bump(m_orbitSign, sign >= 0.0f ? 1.0f : -1.0f); }
        float OrbitSign() const { return m_orbitSign; }

        void SetFakeHeadOffset(const Vec3& metres) { m_fakeHeadOffset = metres; }
        const Vec3& FakeHeadOffset() const { return m_fakeHeadOffset; }

        // Refreshes the head rotation from the current HMD pose. Called once a frame.
        void UpdateFromHeadPose(const Mat4& headToStage);

        // Examines a 4-register upload. When it is the scene camera, fills both eye
        // replacements (16 floats each, in the upload's own layout) and returns true.
        // Returns false to leave the upload alone.
        bool TryPatch(const float* uploaded, float* outLeft, float* outRight);

        // The same eye matrices, without TryPatch recording the upload as the scene
        // camera. For re-deriving an already-known camera block under different
        // settings (the sky's infinite distance) mid-frame, where re-recording an
        // older block's matrix would change what the combined path divides by.
        bool TryPatchNoRecord(const float* uploaded, float* outLeft, float* outRight);

        // Same, for a *combined* world-view-projection. Terrain hands the shader one
        // matrix with the camera already multiplied in, so there is no projection to
        // substitute. But if M = WV * P for the camera P we already know, then
        // WV = M * inverse(P) can be recovered and re-emitted as WV * P_eye. The
        // affine-ness of that residual is what confirms the identification.
        bool TryPatchCombined(const float* uploaded, float* outLeft, float* outRight);

        // Any perspective camera at all, whatever its aspect. TryPatch deliberately
        // insists on the scene camera; this is for world geometry drawn under a
        // secondary perspective camera, which still has to follow the head.
        bool PatchAnyPerspective(const float* uploaded, float* outLeft, float* outRight);

        // Matrices DERIVED from the scene projection's inverse - the classic
        // "reconstruct world position from the depth buffer, then transform into light
        // space" constant. Substituting a new projection without rebuilding these leaves
        // the reconstruction wrong, which is what made the whole shadow cascade read as
        // shadowed. Recognised by scene * W being an orthographic light matrix.
        bool TryPatchInverseDerived(const float* uploaded, float* outLeft, float* outRight);

        // Geometry that is effectively infinitely far away - the sky dome. It must turn
        // with the head but never translate: no per-eye offset, so both eyes see it
        // identically and it reads as infinitely distant, and no head displacement, so
        // leaning cannot produce parallax against it.
        void SetInfiniteDistance(bool on) { m_infiniteDistance = on; }

        // Same recovery, but the residual must also look like a rigid placement
        // (perpendicular axes of equal length). Used when searching a shader's whole
        // constant file, where the plain affine test alone accepts far too much.
        bool TryPatchCombinedStrict(const float* uploaded, float* outLeft, float* outRight);

        bool HasSceneMatrix() const { return m_haveSceneMatrix; }

        // Where the game's camera sits in world coordinates, recovered from the
        // matrices on the wire.
        //
        // The residual left after dividing out the projection is world-to-view for
        // anything drawn in world coordinates - terrain and buildings - and
        // object-to-view for a model that had its placement baked in. The two cannot be
        // told apart one at a time, but they can in bulk: every world-space draw yields
        // the same camera position, while each model yields its own. So the position
        // the most draws agree on is the camera's, and models scatter harmlessly.
        bool CameraWorldPosition(Vec3& out) const;

        // Head yaw relative to the recentred origin, in radians, in the game's
        // left-handed convention. Drives the body-locked panel.
        float HeadYaw() const { return m_headYaw; }

        // Stands in for the headset having turned. Everything downstream treats it as a
        // real head turn, which is the point: it exercises the mechanism rather than an
        // imitation of it.
        void SetFakeHeadYaw(float radians) { m_fakeHeadYaw = radians; }
        float FakeHeadYaw() const { return m_fakeHeadYaw; }
        float HeadPitch() const { return m_headPitch; }
        float HeadRoll() const { return m_headRoll; }

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
        //
        // The offset here is in the NEUTRAL (body) frame - it does not turn with the
        // head, which is what the body-locked panel needs. The world path uses a
        // head-frame copy internally; handing that one out is what made the panel
        // slide forward and back with every nod.
        const Mat4& HeadRotation() const { return m_headRotation; }
        const Vec3& HeadOffsetMetres() const { return m_headOffsetNeutral; }

        // Rotation taking a direction in the game camera's view space into the body
        // (neutral) frame the panel lives in. The world reaches the eye as
        // gameView * HeadCorrection and the panel as body * HeadRotation, so the two
        // frames differ by exactly the turn the game's own camera has been given -
        // the identity unless AimCameraAtHead is steering it.
        Mat4 GameViewToBody() const;

        // The rotation from the game camera's view space into the head's: what the
        // world is turned by on its way to the eye. Anything the client builds square
        // to ITS view (billboards) has to be turned back by the inverse of this to end
        // up square to the head.
        Mat4 GameViewToHead() const { return HeadCorrection(); }

        // A body-frame point or direction (metres, left-handed, the frame the interface
        // panel is placed in) in OpenVR's tracking space (right-handed, the space the
        // poses from WaitGetPoses are in). Undoes exactly what UpdateFromHeadPose does to
        // put the head into the body frame: the yaw-only neutral rotation, the neutral
        // position, and the z flip between the two handednesses.
        Vec3 BodyToStage(const Vec3& body, bool isPoint) const;

        // Trace taps for the pitch pipeline: the camera pitch actually baked into this
        // frame's geometry (from the combined-transform residual) and the compensation
        // pitch this frame was corrected with. Comparing the two per frame is how a
        // shake gets attributed instead of theorised about.
        float LastBakedPitch() const { return m_lastBakedPitch; }
        float CompensationPitchUsed() const { return m_gameCameraPitch; }

        // Everything the eye matrices are built from that is not in the upload itself:
        // the head pose (refreshed once a frame), the game camera compensation, and the
        // configuration. Any change bumps the generation and drops every cached result.
        // Call after a configuration reload.
        void InvalidateCaches() { ++m_generation; }

        // Diagnostics for the log.
        unsigned long long PatchedCount() const { return m_patched; }
        unsigned long long RejectedCount() const { return m_rejected; }
        unsigned long long InverseDerivedCount() const { return m_inverseDerivedPatched; }
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

        // The per-eye replacement for a projection, in row-vector form: the headset's
        // frustum, the eye offset and the head correction, with the original depth
        // terms carried over so depth behaves exactly as the client expects.
        // Shared by every path that corrects geometry. See the definitions.
        Mat4 HeadCorrection() const;
        Mat4 ViewCorrection(float offsetX, float offsetY, float offsetZ) const;

        Mat4 BuildEyeProjection(int eye, float nearPlane, float farPlane,
                                const Mat4& original) const;

        // Caching. Every world draw is issued twice (once per eye) and nearly every
        // constant upload is offered to the patch, so the same work was being redone
        // thousands of times a frame: the scene projection's inverse, the per-eye head
        // correction (with its trig), and whole results for the second eye of a draw.
        // All of it depends only on the inputs below, so it is cached against them and
        // the results are bit-for-bit what recomputing would give.
        template <typename T>
        void Bump(T& field, T value)
        {
            if (field != value)
            {
                field = value;
                ++m_generation;
            }
        }
        const Mat4* InverseScene();
        const Mat4& CachedEyeProjection(int eye, float nearPlane, float farPlane,
                                        const Mat4& original);

        unsigned m_generation = 1;

        Mat4 m_inverseSceneSource;
        Mat4 m_inverseScene;
        bool m_inverseSceneValid = false;
        bool m_inverseSceneOk = false;

        struct EyeCacheEntry
        {
            unsigned generation = 0;
            bool infinite = false;
            float nearPlane = 0.0f;
            float farPlane = 0.0f;
            float depth[4] = {};
            Mat4 matrix;
        };
        EyeCacheEntry m_eyeCache[2][2];   // [eye][infinite]

        // Single-entry memos: the second eye of a draw offers exactly the same upload.
        struct ResultMemo
        {
            bool valid = false;
            unsigned generation = 0;
            bool infinite = false;
            bool rigidChecked = false;   // combined only: passed the strict placement test
            float input[16] = {};
            float left[16] = {};
            float right[16] = {};
        };
        ResultMemo m_patchMemo;
        ResultMemo m_combinedMemo;
        Mat4 m_combinedMemoScene;   // the scene matrix the combined memo divided by
        Decoded m_patchMemoDecoded;

        unsigned long long m_patchMemoHits = 0;
        unsigned long long m_combinedMemoHits = 0;
        unsigned long long m_noteCalls = 0;

        float m_sceneAspect = 16.0f / 9.0f;
        Mat4 m_headRotation = Mat4Identity();
        Mat4 m_neutralInverse = Mat4Identity();
        Vec3 m_neutralPosition;
        Vec3 m_headOffset;              // metres, in the head's own frame (world path)
        Vec3 m_headOffsetNeutral;       // metres, in the neutral frame (panel)
        float m_headYaw = 0.0f;
        float m_fakeHeadYaw = 0.0f;
        float m_fakeHeadPitch = 0.0f;
        Vec3 m_fakeHeadOffset;
        bool m_unwindPitchFirst = false;
        float m_orbitSign = 1.0f;
        Vec3 m_orbitDisplacement;
        bool m_yawIsStable = true;
        bool m_haveHeldYaw = false;
        float m_gameCameraPitch = 0.0f;
        float m_lastBakedPitch = 0.0f;
        float m_headPitch = 0.0f;
        float m_headRoll = 0.0f;
        float m_gameCameraYaw = 0.0f;
        float m_gameCameraOrbitRadius = 0.0f;
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

        // Combined world-view-projection path, counted separately. A terrain chunk
        // whose residual fails the affine test is drawn with the game's own narrow
        // camera, so a non-zero reject count here is visible geometry, not noise.
        unsigned long long m_combinedTried = 0;
        unsigned long long m_combinedPatched = 0;
        unsigned long long m_combinedNoScene = 0;
        unsigned long long m_combinedNotAffine = 0;
        unsigned long long m_inverseDerivedPatched = 0;
        bool m_requireRigidResidual = false;
        bool m_infiniteDistance = false;
        Mat4 m_sceneMatrix;              // row-vector form of the scene projection
        bool m_haveSceneMatrix = false;
        float m_sceneNear = 0.0f;
        float m_sceneFar = 0.0f;
        float m_sceneVerticalScale = 0.0f;
        float m_lastAspect = 0.0f;
        float m_lastNear = 0.0f;
        float m_lastFar = 0.0f;

        // Agreement counting for the camera position, reset every frame.
        void NoteWorldView(const Mat4& worldView);
        struct PositionVote
        {
            Vec3 position;
            int count = 0;
        };
        static constexpr int kPositionVotes = 8;
        PositionVote m_positionVotes[kPositionVotes];
        int m_positionVoteCount = 0;
        PositionVote m_lastVotes[kPositionVotes];
        int m_lastVoteCount = 0;
        Vec3 m_cameraPosition;
        bool m_haveCameraPosition = false;
        struct YawVote
        {
            float yaw = 0.0f;
            int count = 0;
        };
        YawVote m_yawVotes[kPositionVotes];
        int m_yawVoteCount = 0;
        float m_cameraYaw = 0.0f;
        int m_cameraYawWeight = 0;
        bool m_haveCameraYaw = false;
        unsigned long long m_residualsSeen = 0;
        unsigned long long m_residualsNearOrigin = 0;
        unsigned long long m_residualsRigid = 0;

    public:
        // WoW's own camera orientation, recovered from the wire. This is both the anchor
        // for finding the camera in memory and the value that would have to be written
        // to aim its culling somewhere else.
        bool CameraYaw(float& out) const
        {
            if (!m_haveCameraYaw) { return false; }
            out = m_cameraYaw;
            return true;
        }
        int CameraYawWeight() const { return m_cameraYawWeight; }

        unsigned long long ResidualsSeen() const { return m_residualsSeen; }
        unsigned long long ResidualsNearOrigin() const { return m_residualsNearOrigin; }
        unsigned long long ResidualsRigid() const { return m_residualsRigid; }
        // The whole tally from the frame just finished. When the winner turns out not
        // to be the camera, what else was in the running is the informative part.
        int VoteCount() const { return m_lastVoteCount; }
        const Vec3& VotePosition(int index) const { return m_lastVotes[index].position; }
        int VoteWeight(int index) const { return m_lastVotes[index].count; }
    };

    ProjectionPatch& Projection();
}
