#pragma once

#include <cstdint>

namespace wowvr
{
    // Locates WoW's camera structure in memory at runtime.
    //
    // Everything the game decides on the CPU - which objects to cull, how to build the
    // sky dome, which level of detail to pick - is computed against its own camera,
    // which is half as wide as the frustum we actually display and points where the
    // mouse points rather than where the head does. Patching the projection on its way
    // to the GPU cannot fix any of that, because by then the culling has happened.
    //
    // The structure is found by matching values rather than by using a published
    // offset: the client is patched, so its addresses cannot be trusted, but the
    // projection matrix we already decode says exactly what the camera's near plane,
    // far plane and field of view must be.
    //
    // Finding those numbers together is only a hypothesis, though - scratch buffers and
    // stale copies hold them too. A candidate is trusted only once it has gone on
    // agreeing with the projection across later frames, which a copy does not do.
    class CameraProbe
    {
    public:
        // Collects every plausible location. Cheap to call once; does nothing after.
        void Scan(float nearPlane, float farPlane, float aspect, float verticalScale);

        // Re-checks the candidates against a fresh projection, dropping the ones that
        // have stopped agreeing. Confirms the camera once a single one survives twice.
        void Verify(float nearPlane, float farPlane, float aspect, float verticalScale);

        // Nudges one surviving candidate and watches whether the projection the game
        // produces moves with it. Stability proves a value is not scratch memory; only
        // this proves it is the value the camera is actually built from. Call once per
        // frame with the projection's current vertical scale.
        void ActiveTest(float currentVerticalScale);

        bool Found() const { return m_fov != nullptr; }
        bool Searching() const { return m_fov == nullptr && m_candidateCount > 0; }

        // The camera's vertical field of view, in radians. Null until found.
        float* FieldOfView() const { return m_fov; }
        float* NearPlane() const { return m_near; }
        float* FarPlane() const { return m_far; }
        float* Aspect() const { return m_aspect; }

        // True when the projection has changed enough that a previous unsuccessful
        // scan was looking for the wrong numbers - moving from the login screen into
        // the world changes both the far plane and the field of view.
        bool ShouldRescan(float farPlane, float verticalScale) const;

        void Forget();

    private:
        static constexpr int kMaxCandidates = 32;
        static constexpr int kMaxScans = 4;

        struct Candidate
        {
            float* farPlane = nullptr;
            float* nearPlane = nullptr;
            float* fov = nullptr;
            float* aspect = nullptr;
            float fovAtScan = 0.0f;
        };

        bool Examine(const uint8_t* base, size_t size,
                     float nearPlane, float farPlane, float aspect, float expectedFov);

        Candidate m_candidates[kMaxCandidates];
        int m_candidateCount = 0;
        int m_verifyPasses = 0;
        int m_scansAttempted = 0;

        // Active-test state: which candidate is currently perturbed, what it held
        // before, and what the projection looked like at the moment it was changed.
        int m_activeIndex = -1;
        float m_activeOriginal = 0.0f;
        float m_activeBaseline = 0.0f;
        int m_activeSettleFrames = 0;
        bool m_activeTestDone = false;
        float m_scannedFar = 0.0f;
        float m_scannedVerticalScale = 0.0f;

        float* m_near = nullptr;
        float* m_far = nullptr;
        float* m_aspect = nullptr;
        float* m_fov = nullptr;
    };

    CameraProbe& Camera();
}
