#pragma once

#include "core/math3d.h"

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

        // Read-only check of the published 3.3.5a (build 12340) camera offsets. Nothing
        // is written and no pointer is followed without first checking it is readable,
        // so this cannot destabilise the client the way the old write-probe did. The
        // decoded field of view from the projection matrix is the ground truth it is
        // validated against.
        void ProbeKnownOffsets(float verticalScale, float aspect);

        // Bounded read-only sweep of the exe's writable static sections for a pointer
        // that lands on a camera-shaped object.
        void ScanStaticsForCamera(float verticalScale);

        // Looks for the camera OBJECT the renderer actually consumes, rather than the
        // setting. Keyed on three values at once - field of view, near plane and far
        // plane - which together are a far more specific signature than any one of them.
        void ScanForCameraObject(float verticalScale, float nearPlane, float farPlane);

        // Reports whether the located field still agrees with the decoded projection.
        void WatchFovField(float verticalScale);

        // Finds the camera by what it DOES rather than by what it holds.
        //
        // Nothing about the camera survives onto the wire - the client hands the GPU
        // vertices that are already in view space, so the only matrix uploaded is the
        // projection, and it carries neither position nor orientation. That removes
        // every value-matching anchor at once.
        //
        // What is left is behaviour. Turning the character must move the camera's
        // orientation, and standing still must leave it alone. So: snapshot every
        // plausible float, then alternate turning and standing still, keeping only the
        // addresses that changed exactly when the character turned. Animation timers and
        // the like are eliminated by the standing-still passes, which is the part that
        // makes this converge.
        // Steers one of the two headings the scan found, by a constant offset applied
        // every frame. which: 0 off, 1 or 2 selects the address.
        void SetHeadingWrite(int which, float offsetRadians);

        // Pins the camera yaw from a dedicated thread, so the write is not tied to a
        // frame boundary. Diagnostic only.
        void HoldHeading(bool on, float offsetRadians);

        // Pins every candidate the differential scan is currently holding. Heap addresses
        // differ from run to run, so a candidate found this session can only be tested
        // this session.
        void HoldSurvivors(bool on, float offsetRadians);
        void ApplyHeadingWrite();

        // The same idea applied to the whole address space rather than just the exe's
        // static data, narrowed a page at a time first so it fits in a 32-bit process.
        // Works back from a found heap address to a static pointer plus offset, which is
        // the only form that survives relaunching.
        void FindPointerChain();

        // The camera's yaw, reached through that chain. Null whenever the pointer is not
        // yet valid, which is normal on a loading screen.
        float* ResolveCameraYaw();
        void VerifyCameraChain();

        // Locates the camera by the object's own layout - three identical angles at
        // +0, +0x144, +0x148 - which needs no pointer chain and survives relaunching.
        float* FindCameraByShape();

        // Locates the camera during ordinary play, without synthesised input and without
        // any hard-coded address.
        //
        // A fixed address cannot work: the object is allocated afresh every launch. A
        // pointer chain looked promising and turned out to be a coincidence - the one
        // static word that pointed into the object on one run led to a field that sat at
        // zero on the next. And the object's layout alone matches hundreds of thousands
        // of places.
        //
        // What does work is narrowing while the player looks around, using the static
        // mirror as ground truth for what the camera's yaw currently is. It re-checks
        // itself continuously afterwards, so a freed or reallocated object is noticed
        // rather than written over.
        void UpdateAutoLocate();
        float* CameraYaw() const { return m_cameraYawField; }
        // Only once confirmed. A candidate under confirmation has an address but must not
        // be written to - that is the entire purpose of the confirmation phase.
        bool CameraLocated() const;

        // Whether the last attempt gave up. The search is stochastic - the camera's page
        // has to survive five rounds of scoring - and it fails perhaps one run in seven,
        // so the caller retries rather than leaving the feature silently off.
        bool LocateFailed() const;
        void ResetLocate();

        // Turns the game's camera to follow the head, so its culling and streaming follow
        // too. Whatever this has added must be taken back out of the projection we
        // substitute, or the head rotation lands twice and the world turns at double rate.
        void AimAtHead(float headYawRadians);
        float AppliedYaw() const;

        // The vertical equivalent. Widening the aspect ratio fixes the sides but cannot
        // touch the top and bottom - aspect trades width against height, it does not add
        // any - so the only way to have geometry where the head looks up is to move the
        // client's 58.9-degree band up with it.
        float CameraPitch() const;
        void AimPitchAtHead(float headPitchRadians);
        float AppliedPitch() const;

        // Finds the orbit radius. Turning the camera's yaw swings it around the character
        // rather than pivoting it, so the radius is what the resulting displacement has to
        // be cancelled by. It lives in the same object, so this only watches a window
        // around the yaw rather than searching memory again.
        void ZoomSnapshot();
        void ZoomReport(const char* label);

        // The third-person orbit radius in yards, measured to sit 4 bytes before the yaw
        // with a copy at +0xCC. Zero in first person, and zero whenever the camera has not
        // been located or the two copies disagree - a caller can then skip the orbit
        // compensation entirely rather than apply a wrong one.
        float OrbitRadius() const;

        // The field of view the RENDERER reads, as opposed to the setting.
        //
        // The global at 0x00ABFC38 is correctly identified and completely inert: writing it
        // changes nothing on screen, because the client copies it into the active camera
        // once a frame and reads the copy. That copy has never been findable - until now,
        // because the camera object itself is located, so the copy must be inside it.
        // Searching a few kilobytes for a known value is a far easier problem than
        // searching the address space for an unknown one.
        void ScanCameraObjectForFov(float expectedHalfFov);

        // Every location holding a given value. Used with the widened field of view, which
        // is distinctive enough to act as its own marker.
        void ScanForFovCopies(float wanted);

        // Multiplies whatever those fields hold, every frame. 0 restores them.
        void SetFovOverride(float factor);
        void ApplyFovOverride();

        // Finds the camera by moving it deliberately rather than waiting for the player.
        // The view swings for a few seconds and is then put back, which is why it is
        // triggered by hand rather than run on its own.
        void BeginCalibration();
        void UpdateCalibration();
        bool Calibrating() const { return m_calState != CalibrationState::Idle; }

        // One pass over memory for the camera object's full measured layout - a yaw
        // repeated at +0/+0x144/+0x148 and an orbit radius repeated at -0x4/+0xCC. Logs
        // how many places match, which is what says whether behaviour narrowing is needed
        // at all.
        void ScanForCameraSignature();

        void SnapshotPages();
        void FilterPages(bool expectedToChange);
        void PromotePagesToFloats();

        void SnapshotDifferential();

        // direction: 0 stood still, +1 turned right, -1 turned left.
        void FilterDifferential(int direction);
        void ReportDifferential();
        int DifferentialCandidates() const;

        // Read-only watch on the structure that owns the field of view.
        //
        // The setting itself turned out to be inert - the renderer reads a copy - but
        // the struct holding it is the best lead there is, so this reports which of its
        // floats change from frame to frame and by how much. Per-frame camera state
        // (position, orientation) moves as the player does; settings do not. That is
        // what separates them, and no published offset has to be trusted.
        void WatchCameraStruct(const Vec3& cameraPosition, bool havePosition);

        // Widens the game's own field of view so that it culls, streams tiles and sizes
        // its shadow frustum for what VR actually displays. Returns true once the field
        // is under our control. The original is kept and put back on shutdown.
        bool WidenGameFov(float desiredHalfAngleRadians);
        bool WidenGameFovByFactor(float factor);
        void RestoreGameFov();

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
        float* m_fovField = nullptr;
        float m_originalHalfFov = 0.0f;
        bool m_fovWidened = false;
        float m_scannedFar = 0.0f;
        float m_scannedVerticalScale = 0.0f;

        enum class LocateState { WaitingForMotion, NarrowingPages, NarrowingFloats,
                                 Confirming, Verifying, Locked, Failed };
        void UpdateVerification();
        int m_verifyDelay = 0;
        LocateState m_locateState = LocateState::WaitingForMotion;
        int m_locateTick = 0;
        int m_locatePasses = 0;
        float m_lastMirrorYaw = 0.0f;
        float* m_cameraYawField = nullptr;

        bool CameraIsMoving() const;
        void FilterByBehaviour(bool expectedToChange);
        void SelectScoredPages(int motionRounds);
        void CollectLayoutCandidates();
        bool m_wasMoving = false;
        int m_motionRounds = 0;
        enum class CalibrationState { Idle, Nudging, Settling, Pausing };
        static constexpr int kCalibrationPageRounds = 5;
        static constexpr int kCalibrationRounds = 6;
        int m_calPhase = 0;             // 0 narrowing pages, 1 narrowing floats
        CalibrationState m_calState = CalibrationState::Idle;
        int m_calFrame = 0;
        int m_calRound = 0;
        int m_calNudged = 0;
        bool m_calRestoring = false;
        void FinishCalibration();
        void FilterCalibrationRound();
        float* MostConsistentCandidate(int rounds) const;

        void DiscardCandidate(const float* address);
        int m_confirmMoves = 0;
        int m_confirmFailures = 0;
        float m_confirmLastValue = 0.0f;
        float m_appliedYaw = 0.0f;
        float m_lastAimWritten = 0.0f;
        bool m_haveAimed = false;
        float m_appliedPitch = 0.0f;
        float m_lastPitchWritten = 0.0f;
        bool m_havePitched = false;
        int PageCount() const;
        float* FirstCandidate() const;
        float* CandidateAt(int index) const;
        float* FindLayoutMatch(bool requireBehaviourScore) const;

        float* m_near = nullptr;
        float* m_far = nullptr;
        float* m_aspect = nullptr;
        float* m_fov = nullptr;
    };

    CameraProbe& Camera();
}
