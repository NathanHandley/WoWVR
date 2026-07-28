#include "game/game_camera.h"

#include "core/log.h"

#include <windows.h>

#include <cmath>

namespace wowvr
{
    namespace
    {
        // Rebased against the running image, so a relocated client still works.
        const uintptr_t kPublishedImageBase = 0x00400000u;
        const uintptr_t kWorldFrameRva = 0x00B7436Cu - kPublishedImageBase;
        const uintptr_t kActiveCameraOffset = 0x7E20u;

        // Measured from the client's own code; see the header for the disassembly.
        //
        // The two distances are the setting and the live value: +0xB8 read back exactly
        // 12.000000, which is the cameraDistance in Config.wtf, while +0x118 held 11.995192
        // - the smoothed value still converging on it. The live one is what the camera is
        // actually at, and the pair agreeing is a free check that this is the right object.
        const uintptr_t kOrbitRadius = 0x118u;
        const uintptr_t kOrbitRadiusSetting = 0x0B8u;
        const uintptr_t kFreeLookYaw = 0x12Cu;
        const uintptr_t kCameraPitch = 0x130u;
        const uintptr_t kFreeLookPitch = 0x134u;
        // Added to the pitch on the way to the camera, and also subtracted from the clamp
        // limit at 0x00602306. Both uses are consistent with it being a bias rather than a
        // pure limit.
        const uintptr_t kPitchBias = 0x120u;
        const uintptr_t kYawSmoothing = 0x0ACu;
        const uintptr_t kPitchSmoothing = 0x0B0u;

        // Found by dumping the object and looking for values already known from the
        // projection on the wire. +0x38 and +0x3C came back as exactly the near and far
        // planes, which is what proves this object is the camera; the two either side of
        // them are then unmistakable - 1.570796 is pi/2, and 2.962963 is 3200/1080, the
        // exact aspect of the client's own backbuffer.
        //
        // This is the field-of-view copy the renderer reads. The global at 0x00ABFC38 is
        // the setting, and writing it has always been inert because the client copies it
        // in here once a frame and reads the copy. Nobody had found the copy before.
        const uintptr_t kCullNear = 0x038u;
        const uintptr_t kCullFar = 0x03Cu;
        const uintptr_t kCullFov = 0x040u;
        const uintptr_t kCullAspect = 0x044u;

        // The client's own clamp, from 0x006045E9.
        const float kPitchLimit = 1.55334f;

        // The conditional that decides whether the camera collides with the world, inside
        // the only function that shortens the camera distance. See the header.
        //
        // The jump is TO the collision code and the fall-through is the early return, which
        // is the opposite of how it reads. Turning the conditional into an unconditional
        // jump therefore makes collision permanent rather than removing it - which is
        // exactly what the first attempt did, and it measured as no change at all because
        // the flag was already set. Removing the jump is what takes the early return.
        const uintptr_t kCollisionBranchRva = 0x00605DCDu - kPublishedImageBase;
        const uint8_t kOriginalBranch[2] = { 0x74u, 0x16u };     // je 0x00605DE5
        const uint8_t kPatchedBranch[2] = { 0x90u, 0x90u };      // nop nop - fall through

        bool Readable(const void* address, size_t size)
        {
            if (address == nullptr)
            {
                return false;
            }

            MEMORY_BASIC_INFORMATION info;
            if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info))
            {
                return false;
            }

            if (info.State != MEM_COMMIT)
            {
                return false;
            }

            const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
                                 | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                                 | PAGE_EXECUTE_WRITECOPY;
            if ((info.Protect & readable) == 0 || (info.Protect & PAGE_GUARD) != 0)
            {
                return false;
            }

            const uintptr_t start = reinterpret_cast<uintptr_t>(address);
            const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
            return start + size <= end;
        }

        bool Writable(const void* address, size_t size)
        {
            if (!Readable(address, size))
            {
                return false;
            }

            MEMORY_BASIC_INFORMATION info;
            VirtualQuery(address, &info, sizeof(info));
            const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY
                                 | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            return (info.Protect & writable) != 0;
        }

        float ReadFloat(const uint8_t* base, uintptr_t offset)
        {
            return *reinterpret_cast<const float*>(base + offset);
        }

        int ReadInt(const uint8_t* base, uintptr_t offset)
        {
            return *reinterpret_cast<const int*>(base + offset);
        }

        bool SaneAngle(float value)
        {
            // Finite, and within a few turns. The client wraps these, so anything far
            // outside a full circle says we are not looking at an angle.
            return std::isfinite(value) && fabsf(value) < 20.0f;
        }
    }

    GameCamera& GameCam()
    {
        static GameCamera instance;
        return instance;
    }

    bool GameCamera::Plausible(const uint8_t* camera) const
    {
        if (!Readable(camera, 0x280))
        {
            return false;
        }

        // Three angles the client keeps together, and the smoothing counters that gate
        // the two offsets. Checking all five is what separates the camera from any other
        // object that happens to hold a small float at +0x12C.
        if (!SaneAngle(ReadFloat(camera, kFreeLookYaw))
            || !SaneAngle(ReadFloat(camera, kCameraPitch))
            || !SaneAngle(ReadFloat(camera, kFreeLookPitch)))
        {
            return false;
        }

        const int yawSmoothing = ReadInt(camera, kYawSmoothing);
        const int pitchSmoothing = ReadInt(camera, kPitchSmoothing);
        if (yawSmoothing < -1000 || yawSmoothing > 100000
            || pitchSmoothing < -1000 || pitchSmoothing > 100000)
        {
            return false;
        }

        const float radius = ReadFloat(camera, kOrbitRadius);
        const float setting = ReadFloat(camera, kOrbitRadiusSetting);
        return std::isfinite(radius) && radius >= 0.0f && radius < 200.0f
            && std::isfinite(setting) && setting >= 0.0f && setting < 200.0f;
    }

    bool GameCamera::Update()
    {
        const uintptr_t imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (imageBase == 0)
        {
            m_camera = nullptr;
            return false;
        }

        const void* worldFrameSlot = reinterpret_cast<const void*>(imageBase + kWorldFrameRva);
        if (!Readable(worldFrameSlot, sizeof(uintptr_t)))
        {
            m_camera = nullptr;
            return false;
        }

        const uintptr_t worldFrame = *static_cast<const uintptr_t*>(worldFrameSlot);
        if (worldFrame == 0
            || !Readable(reinterpret_cast<const void*>(worldFrame + kActiveCameraOffset),
                         sizeof(uintptr_t)))
        {
            // Normal on a loading screen: the world frame exists only while a world does.
            m_camera = nullptr;
            return false;
        }

        const uintptr_t camera =
            *reinterpret_cast<const uintptr_t*>(worldFrame + kActiveCameraOffset);

        uint8_t* candidate = reinterpret_cast<uint8_t*>(camera);
        if (camera == 0 || !Plausible(candidate) || !Writable(candidate + kFreeLookYaw, 4))
        {
            if (m_camera != nullptr)
            {
                WOWVR_INFO("Game camera at 0x%08X is no longer valid; releasing it.",
                           static_cast<unsigned>(reinterpret_cast<uintptr_t>(m_camera)));
            }
            m_camera = nullptr;
            ++m_failures;
            return false;
        }

        if (m_camera != candidate)
        {
            m_camera = candidate;
            m_appliedYaw = 0.0f;
            m_appliedPitch = 0.0f;

            // Captured before anything is written to it. A fresh camera object means a
            // fresh baseline, and taking it from the live field after we have already
            // widened it would scale an already-scaled value.
            const float fov = ReadFloat(candidate, kCullFov);
            m_baseCullFov = (std::isfinite(fov) && fov > 0.1f && fov < 3.0f) ? fov : 0.0f;
            m_widenLogged = false;

            WOWVR_INFO("Game camera resolved at 0x%08X via *(0x%08X)+0x%X. "
                       "free-look yaw %.4f, pitch %.4f, camera pitch %.4f, orbit %.2f yards.",
                       static_cast<unsigned>(camera),
                       static_cast<unsigned>(imageBase + kWorldFrameRva),
                       static_cast<unsigned>(kActiveCameraOffset),
                       ReadFloat(m_camera, kFreeLookYaw),
                       ReadFloat(m_camera, kFreeLookPitch),
                       ReadFloat(m_camera, kCameraPitch),
                       ReadFloat(m_camera, kOrbitRadius));

            if (!m_announced)
            {
                m_announced = true;
                WOWVR_INFO("Head-driven culling is live: the client will cull, stream and "
                           "light for where the headset looks, not where the character faces.");
            }
        }

        return true;
    }

    void GameCamera::SetFreeLook(float yawRadians, float pitchRadians)
    {
        if (m_camera == nullptr)
        {
            return;
        }

        if (!std::isfinite(yawRadians) || !std::isfinite(pitchRadians))
        {
            return;
        }

        // Kept inside the client's own limit rather than relying on it to clamp: the clamp
        // lives in one of several paths through the camera update, and a value outside the
        // range it expects is not worth finding out about the hard way.
        //
        // Clamped on the pitch we want the camera to END UP at, before the bias below is
        // folded in, because that end angle is what the client limits and what the
        // compensation has to take back out.
        if (pitchRadians > kPitchLimit) { pitchRadians = kPitchLimit; }
        if (pitchRadians < -kPitchLimit) { pitchRadians = -kPitchLimit; }

        // The client adds +0x120 to the pitch on the way to the camera, so asking for p
        // produced a camera sitting at p - 0.0705 rad. Measured by reading the camera's own
        // basis vectors back: asked -34.4 deg, got -38.5; asked -68.8, got -72.9; a constant
        // 4.1 degrees, and +0x120 reads 0.0705 rad, which is 4.04 of them.
        //
        // It was a constant error at every angle, small enough to pass as "looking up and
        // down works", and it meant the compensation was always removing a rotation slightly
        // different from the one the client had applied. Read live rather than baked in,
        // because it is a field and nothing says it holds still.
        const float pitchBias = ReadFloat(m_camera, kPitchBias);
        const float biased = std::isfinite(pitchBias) && fabsf(pitchBias) < 1.0f
                           ? pitchRadians + pitchBias
                           : pitchRadians;

        // A running smoothed transition makes the client ignore these fields entirely.
        // That is a handful of frames after a camera-mode change, and the view snapping
        // back to the character for those frames is exactly the "sometimes it does not
        // work" that a search-based approach could never explain. Ending the transition
        // early costs nothing: the counter only decides how many more frames to
        // interpolate over.
        if (Writable(m_camera + kYawSmoothing, 4))
        {
            int* yawSmoothing = reinterpret_cast<int*>(m_camera + kYawSmoothing);
            int* pitchSmoothing = reinterpret_cast<int*>(m_camera + kPitchSmoothing);
            if (*yawSmoothing > 0 || *pitchSmoothing > 0)
            {
                ++m_suppressedFrames;
                *yawSmoothing = 0;
                *pitchSmoothing = 0;
            }
        }

        // Written in the CLIENT's convention, remembered in OURS. The compensation has to
        // take back out the rotation that actually happened to the geometry, not the number
        // that was written, and the two differ by the sign of the field.
        *reinterpret_cast<float*>(m_camera + kFreeLookYaw) = yawRadians * m_yawSign;
        *reinterpret_cast<float*>(m_camera + kFreeLookPitch) = biased * m_pitchSign;

        // The UNbiased, clamped angle: what the camera actually ends up rotated by, which is
        // what has to come back out of the projection. The biased value is only what the
        // client needs to be told to get there.
        m_appliedYaw = yawRadians;
        m_appliedPitch = pitchRadians;
    }

    void GameCamera::SetSigns(float yawSign, float pitchSign)
    {
        m_yawSign = yawSign >= 0.0f ? 1.0f : -1.0f;
        m_pitchSign = pitchSign >= 0.0f ? 1.0f : -1.0f;
        WOWVR_INFO("Camera offset signs: yaw %+.0f, pitch %+.0f.", m_yawSign, m_pitchSign);
    }

    void GameCamera::SetCullFrustum(float fovRadians, float aspect)
    {
        if (m_camera == nullptr || !Writable(m_camera + kCullFov, 8))
        {
            return;
        }

        // Whether these survive the client's next frame is an open question - it may well
        // recompute both from the cvar and the backbuffer size - so the values read back
        // are logged once rather than assumed to have stuck.
        // Measured: the client builds a vertical field of view of exactly 0.6 * this, and
        // derives the horizontal from that and the window's aspect. So covering a headset
        // that needs 109 degrees vertically takes a value above pi, which looks absurd for
        // a "field of view" and is simply what the arithmetic asks for. The old ceiling of
        // 3.0 was ours, not the client's, and it silently capped the sweep that measured
        // this at 99.7 degrees.
        if (fovRadians > 0.1f && fovRadians < 4.0f)
        {
            *reinterpret_cast<float*>(m_camera + kCullFov) = fovRadians;
        }

        if (aspect > 0.1f && aspect < 20.0f)
        {
            *reinterpret_cast<float*>(m_camera + kCullAspect) = aspect;
        }
    }

    void GameCamera::SetCullWiden(float scale)
    {
        if (m_camera == nullptr || m_baseCullFov <= 0.0f
            || !Writable(m_camera + kCullFov, 4))
        {
            return;
        }

        if (!std::isfinite(scale) || scale < 1.0f) { scale = 1.0f; }

        // The client's own rendering came apart at very wide angles in earlier work, so
        // this stops well short of that rather than trusting the caller's number.
        float widened = m_baseCullFov * scale;
        if (widened > 3.4f) { widened = 3.4f; }

        *reinterpret_cast<float*>(m_camera + kCullFov) = widened;

        if (!m_widenLogged || m_widenScale != scale)
        {
            m_widenLogged = true;
            m_widenScale = scale;
            WOWVR_INFO("Cull frustum widened %.2fx: field of view %.4f -> %.4f rad. The "
                       "client now culls and streams past what is displayed, so a head turn "
                       "cannot outrun it between the write and the next frame.",
                       scale, m_baseCullFov, widened);
        }
    }

    float GameCamera::FreeLookYawField() const
    {
        return m_camera != nullptr ? ReadFloat(m_camera, kFreeLookYaw) : 0.0f;
    }

    float GameCamera::FreeLookPitchField() const
    {
        return m_camera != nullptr ? ReadFloat(m_camera, kFreeLookPitch) : 0.0f;
    }

    // The classic 3.3.5a layout, and the dump agrees: +0x14 read (0.9105, -0.4119, ~0) with
    // +0x20 at (0.4119, 0.9112, ~0) - two perpendicular unit vectors, which is a basis and
    // not a coincidence.
    bool GameCamera::CameraFacing(float& yawRadians, float& pitchRadians) const
    {
        const uintptr_t kForward = 0x14u;

        if (m_camera == nullptr || !Readable(m_camera + kForward, 12))
        {
            return false;
        }

        const float fx = ReadFloat(m_camera, kForward);
        const float fy = ReadFloat(m_camera, kForward + 4);
        const float fz = ReadFloat(m_camera, kForward + 8);

        const float length = sqrtf(fx * fx + fy * fy + fz * fz);
        if (!std::isfinite(length) || length < 0.5f || length > 2.0f)
        {
            return false;
        }

        float sinPitch = fz / length;
        if (sinPitch > 1.0f) { sinPitch = 1.0f; }
        if (sinPitch < -1.0f) { sinPitch = -1.0f; }

        // WoW is Z-up.
        yawRadians = atan2f(fy, fx);
        pitchRadians = asinf(sinPitch);
        return true;
    }

    bool GameCamera::OrbitDisplacementView(float appliedYaw, Vec3& out) const
    {
        out.x = 0.0f;
        out.y = 0.0f;
        out.z = 0.0f;

        if (m_camera == nullptr || !Readable(m_camera + 0x14u, 0x24))
        {
            return false;
        }

        const float radius = OrbitRadius();
        if (radius <= 0.01f)
        {
            return true;        // first person: the camera pivots in place, nothing moved
        }

        float yaw = 0.0f, pitch = 0.0f;
        if (!CameraFacing(yaw, pitch))
        {
            return false;
        }

        // Where the camera sits with the head NEUTRAL, which is the position the viewpoint
        // has to be held at. Not "the camera with our writes removed", which is a different
        // place and not one anything should be anchored to.
        //
        // The two fields do not behave the same way, which is the whole reason this needed
        // measuring rather than deriving. Sampled against the camera's own basis:
        //
        //   yaw    base -20.05 deg, wrote +34.38  ->  +14.34 deg      an OFFSET
        //   pitch  base -49.21 deg, wrote   0.00  ->   -0.01 deg      ABSOLUTE
        //          base -49.21 deg, wrote -49.87  ->  +45.82 deg
        //          base -49.21 deg, wrote +41.80  ->  -45.84 deg
        //
        // so the pitch we write REPLACES whatever the client had, and the camera's pitch is
        // simply the head's. The reference pitch is therefore zero - the level camera - and
        // subtracting an applied pitch from the current one, as this did, reconstructs a
        // reference mirrored to the far side of level. The magnitude survives that (the swing
        // is 2R*sin(theta/2) either way, and it measured 12.45 yards against a predicted
        // 12.45), which is exactly why it went unnoticed. Only the vertical direction flips -
        // and vertical was the axis a chase camera was seen flying along.
        //
        // One consequence worth naming: with the camera aimed, the mouse can no longer pitch
        // the view, because the head owns that field outright.
        const float yaw0 = yaw - appliedYaw * m_yawSign;
        const float pitch0 = 0.0f;

        // Z-up world frame, same as the client's.
        const float cp = cosf(pitch), cp0 = cosf(pitch0);
        const float fx = cp * cosf(yaw), fy = cp * sinf(yaw), fz = sinf(pitch);
        const float f0x = cp0 * cosf(yaw0), f0y = cp0 * sinf(yaw0), f0z = sinf(pitch0);

        // Camera sits at centre - radius * forward, so moving the forward vector moves the
        // camera by radius times the difference.
        const float dx = radius * (f0x - fx);
        const float dy = radius * (f0y - fy);
        const float dz = radius * (f0z - fz);

        // Onto the camera's own axes: forward +0x14, up +0x2C. That +0x2C is the up vector is
        // not assumed - its z component read 0.999, and +0x14's z tracked the requested pitch
        // exactly, which a sideways axis never would.
        const float ux = ReadFloat(m_camera, 0x2Cu);
        const float uy = ReadFloat(m_camera, 0x30u);
        const float uz = ReadFloat(m_camera, 0x34u);

        // The sideways axis is CONSTRUCTED, not read from +0x20.
        //
        // +0x20 is the vector a right-handed Z-up basis puts second, which is up x forward -
        // the LEFT vector, not the right one. Face east with z up and it comes out as +y,
        // which is north. Reading it and calling it "right" put a minus sign into every
        // sideways correction, and it was measured: a 0.6 rad turn gave -9.03 on that axis
        // where the geometry says the camera moved 9.03 yards to its right.
        //
        // forward x up is the right vector by construction in any right-handed frame, so
        // building it here settles the question rather than encoding an answer to it.
        const float rx = fy * uz - fz * uy;
        const float ry = fz * ux - fx * uz;
        const float rz = fx * uy - fy * ux;

        out.x = dx * rx + dy * ry + dz * rz;
        out.y = dx * ux + dy * uy + dz * uz;
        out.z = dx * fx + dy * fy + dz * fz;

        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
    }

    void GameCamera::PinOrbitRadius(bool on)
    {
        if (!on || m_camera == nullptr || !Writable(m_camera + kOrbitRadius, 4))
        {
            return;
        }

        const float wanted = ReadFloat(m_camera, kOrbitRadiusSetting);
        if (!std::isfinite(wanted) || wanted <= 0.01f || wanted > 60.0f)
        {
            return;     // first person, or a value that is not a distance
        }

        *reinterpret_cast<float*>(m_camera + kOrbitRadius) = wanted;
    }

    bool GameCamera::CameraPose(Vec3& position, Vec3& forward, Vec3& up) const
    {
        position.x = position.y = position.z = 0.0f;
        forward.x = forward.y = forward.z = 0.0f;
        up.x = up.y = up.z = 0.0f;

        if (m_camera == nullptr || !Readable(m_camera + 0x08u, 0x30))
        {
            return false;
        }

        position.x = ReadFloat(m_camera, 0x08u);
        position.y = ReadFloat(m_camera, 0x0Cu);
        position.z = ReadFloat(m_camera, 0x10u);
        forward.x = ReadFloat(m_camera, 0x14u);
        forward.y = ReadFloat(m_camera, 0x18u);
        forward.z = ReadFloat(m_camera, 0x1Cu);
        up.x = ReadFloat(m_camera, 0x2Cu);
        up.y = ReadFloat(m_camera, 0x30u);
        up.z = ReadFloat(m_camera, 0x34u);

        return std::isfinite(position.x) && std::isfinite(position.y)
            && std::isfinite(position.z);
    }

    bool GameCamera::SetCollision(bool enabled)
    {
        const uintptr_t imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (imageBase == 0)
        {
            return false;
        }

        uint8_t* site = reinterpret_cast<uint8_t*>(imageBase + kCollisionBranchRva);
        if (!Readable(site, 2))
        {
            WOWVR_WARN("Camera collision: 0x%08X is not mapped.",
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(site)));
            return false;
        }

        const uint8_t* wanted = enabled ? kOriginalBranch : kPatchedBranch;
        if (site[0] == wanted[0] && site[1] == wanted[1])
        {
            m_collisionPatched = !enabled;
            return true;
        }

        // Anything other than the two forms this patch moves between means the code is not
        // what it was read from, and writing would be corruption rather than a patch.
        const bool original = site[0] == kOriginalBranch[0] && site[1] == kOriginalBranch[1];
        const bool patched = site[0] == kPatchedBranch[0] && site[1] == kPatchedBranch[1];
        if (!original && !patched)
        {
            WOWVR_WARN("Camera collision: 0x%08X holds %02X %02X, expected %02X %02X or "
                       "%02X %02X. This is not the client this was decoded from; leaving it "
                       "alone.",
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(site)),
                       site[0], site[1], kOriginalBranch[0], kOriginalBranch[1],
                       kPatchedBranch[0], kPatchedBranch[1]);
            return false;
        }

        DWORD previous = 0;
        if (!VirtualProtect(site, 2, PAGE_EXECUTE_READWRITE, &previous))
        {
            WOWVR_WARN("Camera collision: could not make 0x%08X writable (%s).",
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(site)),
                       LogSystemError(GetLastError()));
            return false;
        }

        site[0] = wanted[0];
        site[1] = wanted[1];
        FlushInstructionCache(GetCurrentProcess(), site, 2);

        DWORD restored = 0;
        VirtualProtect(site, 2, previous, &restored);

        m_collisionPatched = !enabled;
        WOWVR_INFO("Camera collision %s (0x%08X = %02X %02X).",
                   enabled ? "restored" : "disabled",
                   static_cast<unsigned>(reinterpret_cast<uintptr_t>(site)),
                   wanted[0], wanted[1]);
        return true;
    }

    float GameCamera::BasePitch() const
    {
        return m_camera != nullptr ? ReadFloat(m_camera, kCameraPitch) : 0.0f;
    }

    float GameCamera::PitchLimitField() const
    {
        return m_camera != nullptr ? ReadFloat(m_camera, kPitchBias) : 0.0f;
    }

    float GameCamera::CullFov() const
    {
        return m_camera != nullptr ? ReadFloat(m_camera, kCullFov) : 0.0f;
    }

    float GameCamera::CullAspect() const
    {
        return m_camera != nullptr ? ReadFloat(m_camera, kCullAspect) : 0.0f;
    }

    void GameCamera::ClearFreeLook()
    {
        if (m_camera != nullptr && (m_appliedYaw != 0.0f || m_appliedPitch != 0.0f))
        {
            *reinterpret_cast<float*>(m_camera + kFreeLookYaw) = 0.0f;
            *reinterpret_cast<float*>(m_camera + kFreeLookPitch) = 0.0f;
        }

        m_appliedYaw = 0.0f;
        m_appliedPitch = 0.0f;
    }

    float GameCamera::OrbitRadius() const
    {
        if (m_camera == nullptr)
        {
            return 0.0f;
        }

        const float radius = ReadFloat(m_camera, kOrbitRadius);
        if (!std::isfinite(radius) || radius < 0.0f || radius > 60.0f)
        {
            return 0.0f;
        }

        return radius;
    }

    void GameCamera::DumpObject(float nearPlane, float farPlane, float halfFovRadians)
    {
        if (m_camera == nullptr || m_dumped)
        {
            return;
        }

        m_dumped = true;

        WOWVR_INFO("Camera object dump at 0x%08X. Looking for near %.4f, far %.1f, "
                   "half-fov %.5f rad - all three are already known from the projection on "
                   "the wire, so wherever they turn up inside this object is proof of what "
                   "it is.",
                   static_cast<unsigned>(reinterpret_cast<uintptr_t>(m_camera)),
                   nearPlane, farPlane, halfFovRadians);

        const uintptr_t span = 0x300u;
        if (!Readable(m_camera, span))
        {
            WOWVR_INFO("  object is not readable for 0x%X bytes; dump skipped.",
                       static_cast<unsigned>(span));
            return;
        }

        for (uintptr_t offset = 0; offset + 4 <= span; offset += 4)
        {
            const float value = ReadFloat(m_camera, offset);
            const int asInt = ReadInt(m_camera, offset);

            const char* note = "";
            if (std::isfinite(value))
            {
                if (fabsf(value - nearPlane) < 0.001f) { note = "  <- NEAR PLANE"; }
                else if (farPlane > 1.0f && fabsf(value - farPlane) < 0.5f) { note = "  <- FAR PLANE"; }
                else if (halfFovRadians > 0.0f
                         && fabsf(value - halfFovRadians) < 0.0005f) { note = "  <- HALF FOV"; }
                else if (halfFovRadians > 0.0f
                         && fabsf(value - halfFovRadians * 2.0f) < 0.001f) { note = "  <- FULL FOV"; }
            }

            if (offset == kCullFov) { note = "  <- CULL FOV (renderer's copy)"; }
            if (offset == kCullAspect) { note = "  <- CULL ASPECT"; }
            if (offset == kOrbitRadiusSetting) { note = "  <- orbit radius (setting)"; }
            if (offset == kOrbitRadius) { note = "  <- orbit radius (live)"; }
            if (offset == kFreeLookYaw) { note = "  <- FREE-LOOK YAW (what we write)"; }
            if (offset == kCameraPitch) { note = "  <- camera pitch"; }
            if (offset == kFreeLookPitch) { note = "  <- FREE-LOOK PITCH (what we write)"; }

            WOWVR_INFO("  +0x%03X  %14.6f  0x%08X%s",
                       static_cast<unsigned>(offset), value,
                       static_cast<unsigned>(asInt), note);
        }
    }
}
