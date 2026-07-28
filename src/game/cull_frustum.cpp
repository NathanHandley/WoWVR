#include "game/cull_frustum.h"

#include "core/log.h"

#include <windows.h>

#include <cmath>
#include <cstring>

namespace wowvr
{
    namespace
    {
        // Rebased against the running image, so a relocated client still works.
        const uintptr_t kPublishedImageBase = 0x00400000u;

        // ClipVolume::BuildPlanes. Six bytes are replaced - a five-byte jump and a nop -
        // because the third instruction would otherwise be left cut in half.
        const uintptr_t kBuildPlanesRva = 0x00983E70u - kPublishedImageBase;
        const uint8_t kOriginalEntry[6] =
            { 0x55u, 0x8Bu, 0xECu, 0x83u, 0xECu, 0x0Cu };   // push ebp; mov ebp,esp; sub esp,0xC

        // The view objects the scene-node walk indexes, and their stride. Used only to
        // turn a pointer into a view number for the filter and the report.
        const uintptr_t kViewArrayRva = 0x00CDB168u - kPublishedImageBase;
        const uintptr_t kViewStride = 0xFCu;
        const int kViewArrayCount = 8;

        // The index the scene-node walk multiplies by the stride to pick its view. Read
        // for the report only - which view is live at BUILD time need not be the one live
        // when the walk runs, which is exactly why the filter was settled by turning each
        // view away from the eye rather than by reading this.
        const uintptr_t kSelectedViewRva = 0x00CD8798u - kPublishedImageBase;

        // Six planes at the top of the view object, (nx, ny, nz, d) each, then the eight
        // corner points they were built from.
        const int kPlaneCount = 6;
        const uintptr_t kPlaneStride = 0x10u;
        const int kCornerCount = 8;
        const uintptr_t kCornersOffset = 0x60u;

        // How far the camera may sit from a volume's side planes and still count as being
        // in the same frame as it. World volumes measure -0.0; the nearest thing that is
        // not one measures -26.5.
        const float kWorldSpaceTolerance = 1.0f;

        uint8_t* g_trampoline = nullptr;

        uintptr_t ImageBase()
        {
            static uintptr_t base = 0;
            if (base == 0)
            {
                base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            }
            return base;
        }

        // Which instruction in the client asked for this rebuild. Captured from [esp] on
        // entry, where a function's return address sits before it has pushed anything -
        // the same trick that told four identical-looking readers of the cull field of
        // view apart (CLIENT_INTERNALS.md section 1.4).
        uintptr_t g_lastCaller = 0;

        void __stdcall PlanesBuilt(void* view)
        {
            CullView().OnPlanesBuilt(view, g_lastCaller);
        }

        typedef void(__stdcall* PlanesBuiltFn)(void*);
        PlanesBuiltFn g_planesBuilt = PlanesBuilt;

        // Entered in the middle of a thiscall with the view object in ecx and nothing on
        // the stack. The client's own function clobbers ecx (its first act is
        // mov esi, ecx), so the pointer is kept across the call rather than re-read.
        //
        // Naked, because the whole point is to run the client's six replaced bytes and
        // then its remaining code before anything of ours happens: the planes have to
        // exist before they can be rotated.
        __declspec(naked) void BuildPlanesDetour()
        {
            __asm
            {
                mov  eax, [esp]
                mov  g_lastCaller, eax
                push ecx
                mov  eax, g_trampoline
                call eax
                pop  ecx
                mov  eax, g_planesBuilt
                push ecx
                call eax
                ret
            }
        }

        bool ReadVec3(const uint8_t* base, uintptr_t offset, float out[3])
        {
            const float* source = reinterpret_cast<const float*>(base + offset);
            out[0] = source[0];
            out[1] = source[1];
            out[2] = source[2];
            return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
        }

        // How wide a volume looks from the camera, and whether it is centred on the
        // camera's own forward axis.
        //
        // This is what tells the camera's whole view apart from a volume that has been
        // clipped to something in the world. Both are apexed at the camera and both are in
        // world space, so the test that separates world volumes from carried ones cannot
        // see the difference - and rotating the second kind is what made a WMO vanish
        // while it sat in the middle of the screen.
        struct VolumeShape
        {
            bool valid = false;
            float widestCorner = 0.0f;   // radians between the widest corner and forward
            float meanOffAxis = 0.0f;    // radians between the mean corner and forward
            float nearest = 0.0f;        // yards
            float furthest = 0.0f;       // yards
        };

        VolumeShape MeasureVolume(const uint8_t* view, const float position[3],
                                  const float forward[3])
        {
            VolumeShape shape;

            const float length = sqrtf(forward[0] * forward[0] + forward[1] * forward[1]
                                       + forward[2] * forward[2]);
            if (!(length > 0.5f) || !(length < 2.0f))
            {
                return shape;
            }
            const float fx = forward[0] / length;
            const float fy = forward[1] / length;
            const float fz = forward[2] / length;

            float sum[3] = { 0.0f, 0.0f, 0.0f };
            int counted = 0;

            for (int c = 0; c < kCornerCount; ++c)
            {
                const float* corner =
                    reinterpret_cast<const float*>(view + kCornersOffset + c * 12);
                const float dx = corner[0] - position[0];
                const float dy = corner[1] - position[1];
                const float dz = corner[2] - position[2];
                if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz))
                {
                    return shape;
                }

                const float d = sqrtf(dx * dx + dy * dy + dz * dz);

                // The four near corners of a perspective frustum sit within a fraction of
                // a yard of the apex, so their directions are numerical noise. They carry
                // no angle worth measuring and are left out of both the mean and the span.
                if (counted == 0 || d < shape.nearest) { shape.nearest = d; }
                if (d > shape.furthest) { shape.furthest = d; }
                if (!(d > 1.0f))
                {
                    ++counted;
                    continue;
                }

                const float nx = dx / d;
                const float ny = dy / d;
                const float nz = dz / d;

                float cosAngle = nx * fx + ny * fy + nz * fz;
                if (cosAngle > 1.0f) { cosAngle = 1.0f; }
                if (cosAngle < -1.0f) { cosAngle = -1.0f; }
                const float angle = acosf(cosAngle);
                if (angle > shape.widestCorner) { shape.widestCorner = angle; }

                sum[0] += nx;
                sum[1] += ny;
                sum[2] += nz;
                ++counted;
            }

            const float meanLength = sqrtf(sum[0] * sum[0] + sum[1] * sum[1]
                                           + sum[2] * sum[2]);
            if (meanLength > 0.001f)
            {
                float cosMean = (sum[0] * fx + sum[1] * fy + sum[2] * fz) / meanLength;
                if (cosMean > 1.0f) { cosMean = 1.0f; }
                if (cosMean < -1.0f) { cosMean = -1.0f; }
                shape.meanOffAxis = acosf(cosMean);
            }

            shape.valid = counted == kCornerCount;
            return shape;
        }

        // Columns are (forward, left, up) for a heading of yaw and a pitch of pitch, in
        // the client's Z-up right-handed world. Written out rather than composed from the
        // Mat4 helpers because those are row-vector and four-by-four, and this is three
        // columns used once.
        void BasisOf(float yaw, float pitch, float m[3][3])
        {
            const float cy = cosf(yaw);
            const float sy = sinf(yaw);
            const float cp = cosf(pitch);
            const float sp = sinf(pitch);

            m[0][0] = cp * cy;  m[1][0] = cp * sy;  m[2][0] = sp;    // forward
            m[0][1] = -sy;      m[1][1] = cy;       m[2][1] = 0.0f;  // left
            m[0][2] = -sp * cy; m[1][2] = -sp * sy; m[2][2] = cp;    // up
        }
    }

    bool CullFrustum::BuildTrampoline()
    {
        if (g_trampoline != nullptr)
        {
            return true;
        }

        uint8_t* stub = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (stub == nullptr)
        {
            WOWVR_WARN("Cull frustum: could not allocate the trampoline.");
            return false;
        }

        const uintptr_t resume = ImageBase() + kBuildPlanesRva + sizeof(kOriginalEntry);

        int n = 0;
        memcpy(stub, kOriginalEntry, sizeof(kOriginalEntry));
        n += static_cast<int>(sizeof(kOriginalEntry));
        stub[n++] = 0xE9u;
        const int32_t back = static_cast<int32_t>(
            resume - (reinterpret_cast<uintptr_t>(stub) + n + 4));
        memcpy(stub + n, &back, 4);
        n += 4;

        FlushInstructionCache(GetCurrentProcess(), stub, static_cast<SIZE_T>(n));
        g_trampoline = stub;
        return true;
    }

    bool CullFrustum::ApplyPatch(bool on)
    {
        uint8_t* site = reinterpret_cast<uint8_t*>(ImageBase() + kBuildPlanesRva);

        uint8_t patched[sizeof(kOriginalEntry)];
        patched[0] = 0xE9u;
        const int32_t rel = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&BuildPlanesDetour)
            - (reinterpret_cast<uintptr_t>(site) + 5));
        memcpy(patched + 1, &rel, 4);
        patched[5] = 0x90u;

        const uint8_t* wanted = on ? patched : kOriginalEntry;
        if (memcmp(site, wanted, sizeof(kOriginalEntry)) == 0)
        {
            return true;
        }

        // Applying requires the pristine bytes; restoring accepts the detoured form. A
        // site holding neither is a different client build, and writing five bytes of jump
        // into the middle of somebody else's function is not a mistake worth making.
        const bool pristine = memcmp(site, kOriginalEntry, sizeof(kOriginalEntry)) == 0;
        if (on && !pristine)
        {
            WOWVR_WARN("Cull frustum: 0x%08X does not hold the expected bytes; this is "
                       "not the client the plane builder was decoded from. Leaving it "
                       "alone.",
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(site)));
            return false;
        }
        if (!on && site[0] != 0xE9u)
        {
            return false;
        }

        DWORD previous = 0;
        if (!VirtualProtect(site, sizeof(kOriginalEntry), PAGE_EXECUTE_READWRITE, &previous))
        {
            WOWVR_WARN("Cull frustum: 0x%08X could not be made writable.",
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(site)));
            return false;
        }
        memcpy(site, wanted, sizeof(kOriginalEntry));
        VirtualProtect(site, sizeof(kOriginalEntry), previous, &previous);
        FlushInstructionCache(GetCurrentProcess(), site, sizeof(kOriginalEntry));
        return true;
    }

    bool CullFrustum::Enable(bool on)
    {
        if (on == m_patched)
        {
            return true;
        }

        if (on && !BuildTrampoline())
        {
            return false;
        }

        if (!ApplyPatch(on))
        {
            return false;
        }

        m_patched = on;
        WOWVR_INFO("Head-driven cull frustum %s (plane builder at 0x%08X).",
                   on ? "ON: the client's six culling planes now follow the head, and "
                        "its camera is not written at all"
                      : "off: the client culls against its own heading again",
                   static_cast<unsigned>(ImageBase() + kBuildPlanesRva));
        return true;
    }

    void CullFrustum::SetHeadRotation(float yawRadians, float pitchRadians)
    {
        if (!std::isfinite(yawRadians) || !std::isfinite(pitchRadians))
        {
            return;
        }
        m_headYaw = yawRadians;
        m_headPitch = pitchRadians;
    }

    void CullFrustum::SetCameraObject(const void* camera)
    {
        m_camera = static_cast<const uint8_t*>(camera);
    }

    void CullFrustum::SetViewFilter(int viewIndex)
    {
        m_viewFilter = viewIndex;
        WOWVR_INFO("Cull frustum: rotating %s.",
                   viewIndex < 0 ? "every view object"
                                 : (viewIndex == 1 ? "view 1 (the world view)"
                                                   : "one selected view"));
    }

    void CullFrustum::SetYawSign(float sign)
    {
        // Stored as a multiplier rather than snapped to +/-1, because 0 is the useful
        // third value: it takes the head out of the rotation entirely and leaves only the
        // forced offset, which is the only way to sweep the culling volume against a
        // stationary eye. Nothing else here can hold the head still - the headset is on a
        // turntable, and a captured pair a second apart already differs in half its pixels
        // just from the pan.
        m_yawSign = sign;
        WOWVR_INFO("Cull frustum: yaw sign %+.2f.", m_yawSign);
    }

    void CullFrustum::SetPitchSign(float sign)
    {
        m_pitchSign = sign >= 0.0f ? 1.0f : -1.0f;
        WOWVR_INFO("Cull frustum: pitch sign %+.0f.", m_pitchSign);
    }

    void CullFrustum::SetSkipCaller(uintptr_t caller)
    {
        m_skipCaller = caller;
        WOWVR_INFO("Cull frustum: rebuilds from 0x%08X are left alone.",
                   static_cast<unsigned>(caller));
    }

    void CullFrustum::SetWorldSpaceOnly(bool on)
    {
        m_worldSpaceOnly = on;
        WOWVR_INFO("Cull frustum: turning %s.",
                   on ? "only volumes still in world space"
                      : "every volume, including ones carried into another frame "
                        "(this is the fault, not a setting)");
    }

    void CullFrustum::SetShapeGate(float minSpanDegrees, float maxOffAxisDegrees)
    {
        if (!std::isfinite(minSpanDegrees) || minSpanDegrees < 0.0f)
        {
            minSpanDegrees = 0.0f;
        }
        if (!std::isfinite(maxOffAxisDegrees) || maxOffAxisDegrees <= 0.0f)
        {
            maxOffAxisDegrees = 180.0f;
        }

        m_minSpan = minSpanDegrees * 0.0174532925f;
        m_maxOffAxis = maxOffAxisDegrees * 0.0174532925f;

        WOWVR_INFO("Cull frustum: turning only volumes at least %.1f deg wide and no more "
                   "than %.1f deg off the camera's forward axis%s.",
                   minSpanDegrees, maxOffAxisDegrees,
                   (minSpanDegrees <= 0.0f && maxOffAxisDegrees >= 180.0f)
                       ? " - which is every volume, including ones clipped to a doorway "
                         "(this is the fault, not a setting)"
                       : "");
    }

    void CullFrustum::SetForcedYawDegrees(float degrees)
    {
        m_forcedYaw = degrees * 0.0174532925f;
        WOWVR_INFO("Cull frustum: forcing an extra %.1f deg of turn on the culling volume "
                   "(diagnostic; the view does not follow it).", degrees);
    }

    // What space is this volume expressed in, and where did it come from?
    //
    // The discriminator is the camera itself. A world-space perspective frustum is apexed
    // at the camera position, so its four side planes all pass through that point and the
    // signed distance reads 0.00. A volume that has been transformed into some other frame
    // - a map object's local space, say - has no reason to, and does not.
    //
    // This distinction is the whole difference between a rotation that means something and
    // one that scrambles the volume, and nothing else here can tell them apart.
    void CullFrustum::DumpVolume(const void* view, uintptr_t caller, int index) const
    {
        const uint8_t* v = static_cast<const uint8_t*>(view);
        float position[3];
        float forward[3];
        if (!ReadVec3(m_camera, 0x08u, position) || !ReadVec3(m_camera, 0x14u, forward))
        {
            return;
        }

        float worst = 0.0f;
        for (int p = 0; p < 4; ++p)
        {
            const float* plane = reinterpret_cast<const float*>(v + p * kPlaneStride);
            const float atCamera = plane[0] * position[0] + plane[1] * position[1]
                                 + plane[2] * position[2] + plane[3];
            if (fabsf(atCamera) > fabsf(worst))
            {
                worst = atCamera;
            }
        }

        const VolumeShape shape = MeasureVolume(v, position, forward);
        WOWVR_INFO("  volume 0x%08X (index %2d) built from 0x%08X: side planes up to %.1f "
                   "yards off the camera -> %s space; widest corner %.1f deg off forward, "
                   "mean %.1f deg, corners %.1f to %.1f yards out%s",
                   static_cast<unsigned>(reinterpret_cast<uintptr_t>(view)), index,
                   static_cast<unsigned>(caller), worst,
                   fabsf(worst) < 1.0f ? "WORLD" : "some other",
                   shape.widestCorner * 57.2957795f, shape.meanOffAxis * 57.2957795f,
                   shape.nearest, shape.furthest,
                   shape.valid ? "" : " (shape unreadable)");
    }

    void CullFrustum::OnPlanesBuilt(void* view, uintptr_t caller)
    {
        ++m_buildsThisFrame;
        ++m_totalBuilds;

        // Which view this is, recorded whether or not it gets rotated: "the hook never
        // fired for the view the walk tests against" and "it fired and the rotation was
        // zero" are indistinguishable in the headset and trivially distinguishable here.
        const uintptr_t viewBase = ImageBase() + kViewArrayRva;
        const uintptr_t address = reinterpret_cast<uintptr_t>(view);
        int index = -1;
        if (address >= viewBase
            && address < viewBase + kViewStride * kViewArrayCount
            && ((address - viewBase) % kViewStride) == 0)
        {
            index = static_cast<int>((address - viewBase) / kViewStride);
        }

        Seen* record = nullptr;
        for (int i = 0; i < m_seenCount; ++i)
        {
            if (m_seen[i].view == view && m_seen[i].caller == caller)
            {
                record = &m_seen[i];
                break;
            }
        }
        if (record == nullptr && m_seenCount < kMaxViews)
        {
            record = &m_seen[m_seenCount++];
            record->view = view;
            record->caller = caller;
            record->index = index;
        }
        if (record != nullptr)
        {
            ++record->builds;
            record->lastSelected =
                *reinterpret_cast<const int*>(ImageBase() + kSelectedViewRva);
        }

        if (m_camera == nullptr)
        {
            return;
        }

        // Where the camera is and which way it faces, read live rather than pushed from
        // the frame boundary: this runs during the client's own world update, so the
        // camera has already been moved for the frame these planes belong to. The head
        // pose is the one thing that cannot be read live - it is sampled once a frame in
        // Present - and that one frame of lag is what the widened static frustum covers.
        float position[3];
        float forward[3];
        if (!ReadVec3(m_camera, 0x08u, position) || !ReadVec3(m_camera, 0x14u, forward))
        {
            return;
        }

        // Recorded for every rebuild, rotated or not, because it is the one number that
        // says whether rotating this volume about the camera means anything at all.
        float cameraOffset = 0.0f;
        {
            const uint8_t* v = static_cast<const uint8_t*>(view);
            for (int p = 0; p < 4; ++p)
            {
                const float* plane = reinterpret_cast<const float*>(v + p * kPlaneStride);
                const float atCamera = plane[0] * position[0] + plane[1] * position[1]
                                     + plane[2] * position[2] + plane[3];
                if (fabsf(atCamera) > fabsf(cameraOffset))
                {
                    cameraOffset = atCamera;
                }
            }
        }
        // How wide this volume is and where it points, measured before we touch it. Kept
        // on the record whether or not it gets rotated, because "the volume was too narrow
        // to be the camera's view" and "the volume was never built" are the two answers
        // that look identical from inside the headset.
        const VolumeShape shape =
            MeasureVolume(static_cast<const uint8_t*>(view), position, forward);
        if (record != nullptr)
        {
            record->cameraOffset = cameraOffset;
            record->widestCorner = shape.widestCorner;
            record->meanOffAxis = shape.meanOffAxis;
        }

        if (m_dumpBuilds > 0)
        {
            --m_dumpBuilds;
            DumpVolume(view, caller, index);
        }

        if (!m_active)
        {
            return;
        }
        if (m_viewFilter >= 0 && index != m_viewFilter)
        {
            return;
        }
        if (m_skipCaller != 0 && caller == m_skipCaller)
        {
            return;
        }

        // The gate that matters, and the one that took longest to find: rotate this volume
        // only if it is still in world space.
        //
        // Turning a frustum about the camera position is the frustum of a turned camera
        // ONLY while the volume is expressed in the frame that position belongs to. The
        // client does not keep them all there. It rebuilds the world view 36 times a frame
        // through 0x00983F40, which carries the volume into another frame by a matrix
        // first - measured, the camera ends up 9,840 yards off the result's side planes -
        // and it keeps several heap volumes 26 to 2,195 yards off their own.
        //
        // Rotating those about a point that means nothing to them scrambles the volume,
        // which in the headset is a large WMO vanishing whole while you look straight at
        // it, and vanishing when you turn TOWARD it.
        //
        // The test is self-validating and needs no addresses: a perspective frustum's four
        // side planes meet at its apex, so for a world-space one the camera reads 0.0 from
        // all four. World volumes measured -0.0; the nearest non-world measured -26.5. A
        // yard of tolerance sits in the middle of a gap that wide.
        if (m_worldSpaceOnly && !(fabsf(cameraOffset) < kWorldSpaceTolerance))
        {
            return;
        }

        // The second gate, and the one the Stormwind gate found: rotate this volume only
        // if it is the camera's WHOLE view rather than a view of something in particular.
        //
        // The client builds both, in world space, apexed at the camera, through the same
        // function - so neither the world-space test above nor the calling site tells them
        // apart. What does tell them apart is shape, and the measurement is not close.
        // Standing in the gate's archway:
        //
        //   the camera's own view (three volumes)  70.2 deg wide, mean  0.0 deg off axis
        //   the two clipped to the arch            43.5 and 31.6,  mean 26.1 and 24.2
        //
        // The mean is the sharper of the two and it is exact rather than empirical: a
        // perspective frustum is symmetric about the axis it was built on, so the average
        // of its eight corner directions IS that axis, to the last decimal place. A volume
        // clipped to an opening points at the opening instead.
        //
        // Width is kept as the second half because an opening dead ahead would pass the
        // symmetry test - it would just be narrow, and the widest the camera's own view
        // ever gets is 46 degrees with the cull field of view left unwidened, so 40 has
        // clearance under it either way.
        //
        // Rotating a clipped volume swings it off the opening it was cut to, and what
        // disappears is not at the edge of vision - it is whatever lies beyond the
        // doorway, dead ahead. That is what a large WMO vanishing while you look straight
        // at it actually was.
        //
        // Leaving them alone loses nothing. A clipped volume is the intersection of the
        // camera's view with an opening, so once the view it was cut from has been turned
        // to the head, the cut inherits that turn; the opening's own extent is a fact
        // about the building and has no business following anybody's head.
        const bool shapeGated = m_minSpan > 0.0f || m_maxOffAxis < 3.1415926f;
        if (shapeGated
            && (!shape.valid
                || shape.widestCorner < m_minSpan
                || shape.meanOffAxis > m_maxOffAxis))
        {
            return;
        }

        const float length = sqrtf(forward[0] * forward[0] + forward[1] * forward[1]
                                   + forward[2] * forward[2]);
        if (!(length > 0.5f) || !(length < 2.0f))
        {
            return;
        }

        float sinPitch = forward[2] / length;
        if (sinPitch > 1.0f) { sinPitch = 1.0f; }
        if (sinPitch < -1.0f) { sinPitch = -1.0f; }

        const float yaw0 = atan2f(forward[1], forward[0]);
        const float pitch0 = asinf(sinPitch);

        // The head's turn is an offset on the heading and its pitch composes with the
        // camera's own, which is the whole difference from the aiming path: there the
        // written pitch field REPLACED the client's, so the mouse could no longer pitch
        // the view. Nothing is written here, so the mouse keeps its pitch and the head
        // adds to it - exactly what the eye projection already does with the same two
        // angles.
        const float deltaYaw = m_headYaw * m_yawSign + m_forcedYaw;
        const float deltaPitch = m_headPitch * m_pitchSign;
        const float yaw1 = yaw0 + deltaYaw;
        const float pitch1 = pitch0 + deltaPitch;

        m_lastYawApplied = deltaYaw;
        m_lastPitchApplied = deltaPitch;

        float from[3][3];
        float to[3][3];
        BasisOf(yaw0, pitch0, from);
        BasisOf(yaw1, pitch1, to);

        // R = to * transpose(from): the rotation that carries the camera's own basis onto
        // the one it would have if it were aimed at the head. Orthonormal, so the
        // transpose is the inverse.
        float r[3][3];
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
            {
                r[i][j] = to[i][0] * from[j][0]
                        + to[i][1] * from[j][1]
                        + to[i][2] * from[j][2];
            }
        }

        // Turn the eight CORNERS and let the client rebuild its own planes from them,
        // rather than rewriting the six planes directly.
        //
        // Rewriting the planes is less code and was the first thing tried, but it steers
        // only the volume it is written into. The client derives further volumes from a
        // volume's CORNERS - that is what 0x00983F40 does, 36 times a frame, carrying the
        // world view into another frame for the map-object tests. Corners we never touched
        // meant every one of those derived volumes was built from an unturned frustum, so
        // ADT terrain and WMO buildings went on being culled against the character's
        // heading no matter how far the head turned.
        //
        // Turning the corners puts the aim upstream of the client's own derivation, so
        // everything downstream inherits it and inherits it in the right frame.
        //
        // Safe against compounding because corners are freshly written immediately before
        // every rebuild this gate lets through: those are all 0x0098430C, which is inside
        // SetCorners and has just copied 0x60 bytes of new ones in.
        uint8_t* volume = static_cast<uint8_t*>(view);
        for (int c = 0; c < kCornerCount; ++c)
        {
            float* corner = reinterpret_cast<float*>(volume + kCornersOffset + c * 12);
            const float x = corner[0] - position[0];
            const float y = corner[1] - position[1];
            const float z = corner[2] - position[2];

            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            {
                return;
            }

            corner[0] = position[0] + r[0][0] * x + r[0][1] * y + r[0][2] * z;
            corner[1] = position[1] + r[1][0] * x + r[1][1] * y + r[1][2] * z;
            corner[2] = position[2] + r[2][0] * x + r[2][1] * y + r[2][2] * z;
        }

        // The client's own plane builder, re-entered through the trampoline so it runs its
        // real code rather than our detour. Thiscall with no stack arguments, so __fastcall
        // with an ignored second argument reaches it exactly.
        typedef void(__fastcall * BuildPlanesFn)(void*, void*);
        reinterpret_cast<BuildPlanesFn>(g_trampoline)(view, nullptr);

        ++m_totalRotated;
        if (record != nullptr)
        {
            ++record->rotated;
        }
    }

    void CullFrustum::FrameBoundary()
    {
        m_buildsLastFrame = m_buildsThisFrame;
        m_buildsThisFrame = 0;
    }

    void CullFrustum::Report() const
    {
        WOWVR_INFO("Cull frustum: %s, %s, filter %d, %llu builds (%llu rotated), "
                   "%u rebuilds in the last frame, last applied yaw %.1f deg pitch %.1f deg",
                   m_patched ? "hooked" : "NOT hooked",
                   m_active ? "active" : "idle",
                   m_viewFilter,
                   static_cast<unsigned long long>(m_totalBuilds),
                   static_cast<unsigned long long>(m_totalRotated),
                   m_buildsLastFrame,
                   m_lastYawApplied * 57.2957795f,
                   m_lastPitchApplied * 57.2957795f);

        for (int i = 0; i < m_seenCount; ++i)
        {
            WOWVR_INFO("  view 0x%08X (index %2d) from 0x%08X: %7u builds, %7u rotated, "
                       "camera %8.1f yards off its side planes (%s), widest corner %5.1f "
                       "deg off forward (mean %5.1f) -> %s, client had view %d selected",
                       static_cast<unsigned>(
                           reinterpret_cast<uintptr_t>(m_seen[i].view)),
                       m_seen[i].index,
                       static_cast<unsigned>(m_seen[i].caller),
                       m_seen[i].builds, m_seen[i].rotated,
                       m_seen[i].cameraOffset,
                       fabsf(m_seen[i].cameraOffset) < 1.0f ? "WORLD" : "other frame",
                       m_seen[i].widestCorner * 57.2957795f,
                       m_seen[i].meanOffAxis * 57.2957795f,
                       (m_seen[i].widestCorner >= m_minSpan
                        && m_seen[i].meanOffAxis <= m_maxOffAxis)
                           ? "the camera's own view"
                           : "clipped to something",
                       m_seen[i].lastSelected);
        }

        // A rebuild count of zero while the hook is actually steering means the client is
        // not rebuilding its volumes at all, and the aim is frozen wherever it last
        // landed. Only worth saying when active - a login screen legitimately has none.
        if (m_patched && m_active && m_buildsLastFrame == 0)
        {
            WOWVR_WARN("Cull frustum: no rebuild in the last frame - the aim is as stale "
                       "as the last frame that had one.");
        }
    }

    CullFrustum& CullView()
    {
        static CullFrustum instance;
        return instance;
    }
}
