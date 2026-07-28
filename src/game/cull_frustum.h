#pragma once

#include <cstdint>

namespace wowvr
{
    // Point the client's CULLING frustum where the head looks, without moving its camera.
    //
    // The problem this answers has been constant across the whole project: the client
    // decides what to draw against a frustum built from the character's heading, roughly
    // 54 degrees wide. A VR eye sees 110 and can turn 180. Anything the head looks at that
    // the client did not consider is not dark or low-detail, it is absent.
    //
    // Every previous answer went through the camera object, and each failed for its own
    // reason (VR_PIPELINE.md section 6):
    //
    //   - Aiming the camera continuously culls correctly out to 178 degrees, but the
    //     client bakes its distant pass - sky, backdrop terrain, far WMO canopy - against
    //     a heading that only ADOPTS the free-look offsets over a second or two. Worn on a
    //     moving head that pass permanently lags and the sky drags.
    //   - Aiming on a deadzone removes the drag and replaces it with a visible pop at each
    //     re-aim.
    //   - Disabling culling outright draws everything and costs 114 fps to 68.
    //
    // The common cause is that the camera is not a culling device: it is the thing the
    // client positions the viewpoint with, bakes distant geometry against, and smooths.
    // Writing it to steer culling drags all of that along.
    //
    // So do not write it. The frustum the client actually tests against is six planes
    // sitting in a view object, rebuilt from eight corner points by one function, and
    // rotating those six planes about the camera position produces exactly the volume an
    // aimed camera would have produced - with nothing else changed. The camera keeps the
    // character's heading, so the distant pass is baked against a heading that is not
    // moving and cannot lag; the viewpoint never translates, so there is no orbit swing to
    // measure and cancel; and the volume is the same size as before, merely pointed
    // somewhere else, so the frame cost is unchanged.
    //
    // The client's own code, from a linear sweep of Wow.exe:
    //
    //   0x00983E70  ClipVolume::BuildPlanes(this)      thiscall, no arguments
    //               builds six planes at this+0x00 .. this+0x5F from the eight frustum
    //               corner points at this+0x60 .. this+0xBC. Five come from
    //               0x007912C0 (plane through three points); the sixth is the fifth
    //               negated.
    //   0x00984240  ClipVolume::SetCorners(this, const Vec3 corners[8])
    //               copies 0x60 bytes of corners and calls BuildPlanes. Nine callers.
    //   0x009839E0  ClipVolume::TestBounds(this, const AABB* bounds)
    //               the shared positive-vertex test: six planes at this+0, stride 0x10,
    //               laid out (nx, ny, nz, d), inside when dot(n, p) + d >= 0. Returns 3
    //               for inside and 0 for rejected, and has 23 call sites across terrain,
    //               doodad and WMO code - which is why one plane set covers all of them.
    //   0x007911D0  the scene-node walk, which calls TestBounds against the view object
    //               at 0x00CDB168 + [0x00CD8798] * 0xFC and caches the verdict in node
    //               flag bit 0x2000.
    //
    // Hooking BuildPlanes rather than writing the planes on a timer is what makes this
    // safe against the client's own cadence: each rebuild is rotated exactly once, so the
    // rotation can never compound, and a frame with no rebuild simply keeps the previous
    // frame's aim rather than drifting.
    class CullFrustum
    {
    public:
        // Detours 0x00983E70. Verifies the six bytes it replaces first, so a client this
        // was not decoded from is declined rather than corrupted.
        bool Enable(bool on);
        bool Enabled() const { return m_patched; }

        // Pushed once a frame from the Present hook. Yaw is the head's turn relative to
        // the recentred origin, in the client's own sense (the same value the camera
        // aiming path wrote into the free-look offset); pitch is positive looking up.
        void SetHeadRotation(float yawRadians, float pitchRadians);

        // The camera object, for the position the rotation pivots about and the heading
        // the head's turn is measured from. Re-pushed every frame because the object is
        // destroyed and rebuilt across every loading screen.
        void SetCameraObject(const void* camera);

        // False leaves every plane exactly as the client built it, which is the flat
        // client's behaviour. The detour stays in place; it just stops doing anything.
        void SetActive(bool active) { m_active = active; }

        // Which volume gets rotated. -1, the default, means every one of them, and that is
        // not laziness - it is what the client's own code requires.
        //
        // The shared test at 0x009839E0 has 23 call sites and they fall into three
        // families. Eight index the view array with `[0x00CD8798]`, which is the scene and
        // doodad path. Thirteen, all in 0x007BB000-0x007BD200, test volumes held *inside
        // the map object structures themselves*, at `object+0x2DC` and `object+0x624` -
        // those are built by the same plane builder but live on the heap, not in the view
        // array. One more works from a stack local.
        //
        // So rotating the view array alone steers doodads and game objects and leaves ADT
        // terrain and WMO buildings culled against the character's heading, which in the
        // headset looks like doodads hanging in an empty sky with no ground under them.
        // Confirmed by capture: rotating view 1 only leaves a void band with a tree
        // floating in it; rotating every volume fills the same band with ground and trees.
        //
        // View **1** is the scene view within the array, measured rather than assumed -
        // the site that writes the index at 0x00CD8798 also fills in view 0, which makes
        // view 0 look obvious, and turning each view 180 degrees from the eye in turn
        // shows view 1 taking the world with it while 0, 2 and 3 change nothing visible.
        // Kept selectable because narrowing the set is the first thing to try if rotating
        // everything ever turns out to cost something.
        //
        // That last measurement was taken in Teldrassil, which has no building you can
        // walk into, and it is why views 2 and 3 looked inert: indoors they are the
        // volumes clipped to the doorway you are looking through, and rotating them is a
        // fault. SetMinSpanDegrees is what excludes them now, by width rather than by
        // index, so the set does not have to be right about a particular client's layout.
        void SetViewFilter(int viewIndex);
        int ViewFilter() const { return m_viewFilter; }

        // Which way round the head's turn runs, relative to the heading the client's
        // camera already holds. Exposed rather than baked in for the same reason the
        // camera's own signs are: a sign is the one thing worth being able to flip in the
        // headset without a rebuild, and every sign in this project so far has taken a
        // measurement rather than an argument to settle.
        void SetYawSign(float sign);
        void SetPitchSign(float sign);

        // A fixed extra turn on top of the head's, in degrees, for diagnosis only.
        //
        // It exists because a starting zone answers no question about culling: with the
        // client's own cull field of view cut to 0.5 rad - a 17 degree cone - the picture
        // here did not change at all, because nothing within 50 units is ever culled and
        // in Teldrassil nothing worth seeing is further away than that. Forcing 180
        // degrees points the culling volume at the back of the character's head while the
        // eye still looks forward, so anything the node walk actually gates has to
        // disappear. If the picture survives that, these planes are not the ones the
        // world pass reads, and no amount of looking will show it.
        void SetForcedYawDegrees(float degrees);

        // Leave alone every rebuild that comes from this instruction. 0 skips nothing.
        //
        // The client rebuilds a volume from two places: 0x0098430C, which has just copied
        // fresh world-space corners in, and 0x00983F77, which has just carried the volume
        // it already held into another frame by a matrix. Rotating the first is the whole
        // feature. Rotating the second would apply a world-space turn to a volume that is
        // no longer in world space - and would do it to a volume whose corners already
        // carry the turn, so the head would be counted twice.
        void SetSkipCaller(uintptr_t caller);

        // Off turns volumes that are no longer in world space as well, which is what this
        // did before the fault was understood. Kept only so the two can be compared in the
        // headset without a rebuild; there is no reason to run it.
        void SetWorldSpaceOnly(bool on);

        // What shape a volume has to be before the head is allowed to turn it: at least
        // minSpan degrees from the camera's forward axis out to its widest corner, and no
        // more than maxOffAxis degrees between that axis and the average of its corners.
        // 0 and 180 turn everything, which is the fault this was written to fix.
        //
        // The client builds the camera's whole view and volumes clipped to whatever
        // opening you are looking through, through the same function, in world space,
        // apexed at the camera - so nothing else here can tell them apart. Standing in the
        // Stormwind gate's archway:
        //
        //   the camera's own view   70.2 deg wide, mean  0.0 deg off the forward axis
        //   clipped to the arch     43.5 and 31.6, mean 26.1 and 24.2
        //
        // The mean is exact rather than empirical - a perspective frustum is symmetric
        // about the axis it was built on, so the average of its corners IS that axis - and
        // the width covers the one case symmetry does not, an opening dead ahead.
        void SetShapeGate(float minSpanDegrees, float maxOffAxisDegrees);

        // Logs the filtered view's eight corner points and six planes, as the client built
        // them, against the camera's own position and forward axis - once, on the next
        // build.
        //
        // Rotating a frustum about the camera position is only the frustum of a turned
        // camera if the volume really is apexed there. Nothing so far proves it is: the
        // volume could be a box, or apexed on the character rather than the camera, and
        // either would explain a rotation that removes geometry instead of revealing it
        // while looking exactly like a sign error.
        // Dumps the next few volume rebuilds, whatever they are for, with the calling site
        // and enough geometry to tell what space each one is expressed in: for a
        // world-space frustum apexed at the camera the four side planes read 0.00 yards
        // from the camera position, and for anything else they do not.
        void RequestDump() { m_dumpBuilds = 10; }

        // What has actually been seen and done, which is the only way to tell "the hook
        // is not firing" from "the hook fires and the rotation is zero" from "it fires on
        // a view nothing culls against". Each of those looks identical in the headset.
        void Report() const;

        // Rolls the per-frame counters. Called from the frame boundary so the report can
        // say how many rebuilds a frame actually gets - if that is not one, the aim is
        // stale by however many frames the client skips.
        void FrameBoundary();

        // Called by the detour, on the client's own thread, immediately after the client
        // has written the six planes. Public only because a naked stub cannot reach a
        // private member.
        void OnPlanesBuilt(void* view, uintptr_t caller);

    private:
        bool ApplyPatch(bool on);
        bool BuildTrampoline();
        void DumpVolume(const void* view, uintptr_t caller, int index) const;

        bool m_patched = false;
        bool m_active = false;
        int m_viewFilter = -1;

        float m_headYaw = 0.0f;
        float m_headPitch = 0.0f;
        float m_yawSign = 1.0f;
        float m_pitchSign = 1.0f;
        float m_forcedYaw = 0.0f;
        uintptr_t m_skipCaller = 0;
        bool m_worldSpaceOnly = true;
        float m_minSpan = 40.0f * 0.0174532925f;
        float m_maxOffAxis = 5.0f * 0.0174532925f;
        const uint8_t* m_camera = nullptr;

        // Distinct view objects the builder has been seen writing, so the filter can be
        // chosen from what the client does rather than from what it looked like it ought
        // to do.
        // One entry per (view, calling site) pair rather than per view. The client rebuilds
        // the world view several times a frame, and if those rebuilds come from different
        // call sites serving different purposes then rotating all of them is not one change
        // but several - which is the shape of a fault that pops objects in and out while
        // you look straight at them.
        static const int kMaxViews = 24;
        struct Seen
        {
            const void* view = nullptr;
            uintptr_t caller = 0;
            int index = -1;
            int lastSelected = -1;   // what 0x00CD8798 held when this view was built
            // How far the camera sits from this volume's side planes, before we touch it.
            // ~0 means a world-space frustum apexed at the camera; anything else means the
            // volume has been carried into some other frame, where a world-space rotation
            // about the camera position is meaningless.
            float cameraOffset = 0.0f;
            // How far this volume's widest corner sits off the camera's forward axis, and
            // where the average of its corners points. The camera's own view is wide and
            // centred; a volume clipped to a doorway is narrow, and may be centred too -
            // which is why the width is the test and the mean is only reported.
            float widestCorner = 0.0f;
            float meanOffAxis = 0.0f;
            uint32_t builds = 0;
            uint32_t rotated = 0;
        };
        Seen m_seen[kMaxViews];
        int m_seenCount = 0;

        int m_dumpBuilds = 0;
        uint32_t m_buildsThisFrame = 0;
        uint32_t m_buildsLastFrame = 0;
        uint64_t m_totalBuilds = 0;
        uint64_t m_totalRotated = 0;
        float m_lastYawApplied = 0.0f;
        float m_lastPitchApplied = 0.0f;
    };

    CullFrustum& CullView();
}
