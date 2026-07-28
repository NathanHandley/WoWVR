#include "game/sound_listener.h"

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

        // SetListenerAttributes(position, forward, up, velocity). Six bytes are replaced -
        // a five-byte jump and a nop - because the third instruction would otherwise be
        // left cut in half.
        const uintptr_t kSetListenerRva = 0x004C5B20u - kPublishedImageBase;
        const uint8_t kOriginalEntry[6] =
            { 0x55u, 0x8Bu, 0xECu, 0x8Bu, 0x45u, 0x14u };  // push ebp; mov ebp,esp; mov eax,[ebp+0x14]

        // The camera object's world position, the same field the culling rotation pivots
        // about (game_camera.h, CLIENT_INTERNALS.md section 2.1).
        const uintptr_t kCameraPositionOffset = 0x08u;

        const float kRadiansPerDegree = 0.0174532925f;
        const float kDegreesPerRadian = 57.2957795f;

        // Just short of straight up, so the yaw of the corrected forward axis stays
        // defined. FMOD is handed a forward and an up vector and wants them independent.
        const float kMaxPitch = 1.5533f;   // 89 degrees

        uint8_t* g_trampoline = nullptr;

        typedef int(__cdecl* SetListenerFn)(const float*, const float*, const float*,
                                            const void*);

        uintptr_t ImageBase()
        {
            static uintptr_t base = 0;
            if (base == 0)
            {
                base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            }
            return base;
        }

        // The client's wrapper is cdecl and takes four pointers, so the detour can be an
        // ordinary function - no naked stub and no hand-written prologue, unlike the
        // culling hook, which has to enter in the middle of a thiscall.
        int __cdecl SetListenerDetour(const float* position, const float* forward,
                                      const float* up, const void* velocity)
        {
            return HeadListener().OnSetListener(position, forward, up, velocity);
        }

        bool Finite3(const float* v)
        {
            return v != nullptr
                && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
        }

        // Columns are (forward, left, up) for a heading of yaw and a pitch of pitch, in
        // the client's Z-up right-handed world. The same basis the culling rotation uses,
        // and deliberately so: both turn the same world by the same head, so a sign that
        // is right for one is right for the other.
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

        void Rotate(const float r[3][3], const float* v, float* out)
        {
            out[0] = r[0][0] * v[0] + r[0][1] * v[1] + r[0][2] * v[2];
            out[1] = r[1][0] * v[0] + r[1][1] * v[1] + r[1][2] * v[2];
            out[2] = r[2][0] * v[0] + r[2][1] * v[1] + r[2][2] * v[2];
        }

        void Normalise(float* v)
        {
            const float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (length > 1.0e-6f)
            {
                v[0] /= length;
                v[1] /= length;
                v[2] /= length;
            }
        }
    }

    bool SoundListener::BuildTrampoline()
    {
        if (g_trampoline != nullptr)
        {
            return true;
        }

        uint8_t* stub = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (stub == nullptr)
        {
            WOWVR_WARN("Sound listener: could not allocate the trampoline.");
            return false;
        }

        const uintptr_t resume = ImageBase() + kSetListenerRva + sizeof(kOriginalEntry);

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

    bool SoundListener::ApplyPatch(bool on)
    {
        uint8_t* site = reinterpret_cast<uint8_t*>(ImageBase() + kSetListenerRva);

        uint8_t patched[sizeof(kOriginalEntry)];
        patched[0] = 0xE9u;
        const int32_t rel = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&SetListenerDetour)
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
            WOWVR_WARN("Sound listener: 0x%08X does not hold the expected bytes; this is "
                       "not the client the listener setter was decoded from. Leaving it "
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
            WOWVR_WARN("Sound listener: 0x%08X could not be made writable.",
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(site)));
            return false;
        }
        memcpy(site, wanted, sizeof(kOriginalEntry));
        VirtualProtect(site, sizeof(kOriginalEntry), previous, &previous);
        FlushInstructionCache(GetCurrentProcess(), site, sizeof(kOriginalEntry));
        return true;
    }

    bool SoundListener::Enable(bool on)
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
        WOWVR_INFO("Head-driven sound listener %s (setter at 0x%08X).",
                   on ? "ON: the ears now follow the headset rather than the camera"
                      : "off: the client places its own listener again",
                   static_cast<unsigned>(ImageBase() + kSetListenerRva));
        return true;
    }

    void SoundListener::SetHeadRotation(float yawRadians, float pitchRadians,
                                        float rollRadians)
    {
        if (!std::isfinite(yawRadians) || !std::isfinite(pitchRadians)
            || !std::isfinite(rollRadians))
        {
            return;
        }
        m_headYaw = yawRadians;
        m_headPitch = pitchRadians;
        m_headRoll = rollRadians;
    }

    void SoundListener::SetHeadOffsetMetres(const Vec3& metres)
    {
        if (!std::isfinite(metres.x) || !std::isfinite(metres.y) || !std::isfinite(metres.z))
        {
            return;
        }
        m_headOffset = metres;
    }

    void SoundListener::SetUnitsPerMetre(float units)
    {
        if (std::isfinite(units) && units > 0.0f)
        {
            m_unitsPerMetre = units;
        }
    }

    void SoundListener::SetCameraObject(const void* camera)
    {
        m_camera = static_cast<const uint8_t*>(camera);
    }

    void SoundListener::SetEarsAtCamera(bool on)
    {
        m_earsAtCamera = on;
        WOWVR_INFO("Sound listener: ears at %s.",
                   on ? "the camera, which is where the head is"
                      : "wherever the client put them (the character, by default)");
    }

    void SoundListener::SetTrackPosition(bool on)
    {
        m_trackPosition = on;
        WOWVR_INFO("Sound listener: head displacement %s.",
                   on ? "moves the ears" : "ignored (rotation only)");
    }

    void SoundListener::SetYawSign(float sign)
    {
        // Stored as a multiplier rather than snapped to +/-1, because 0 is the useful
        // third value: it takes the head out of the correction entirely and leaves only
        // the forced offset, which is what makes a fixed, held sound field to listen to.
        m_yawSign = sign;
        WOWVR_INFO("Sound listener: yaw sign %+.2f.", m_yawSign);
    }

    void SoundListener::SetPitchSign(float sign)
    {
        m_pitchSign = sign;
        WOWVR_INFO("Sound listener: pitch sign %+.2f.", m_pitchSign);
    }

    void SoundListener::SetRollSign(float sign)
    {
        m_rollSign = sign;
        WOWVR_INFO("Sound listener: roll sign %+.2f.", m_rollSign);
    }

    void SoundListener::SetLateralSign(float sign)
    {
        m_lateralSign = sign;
        WOWVR_INFO("Sound listener: lateral sign %+.2f (flip this if stepping left moves "
                   "sounds left instead of right).", m_lateralSign);
    }

    void SoundListener::SetForcedYawDegrees(float degrees)
    {
        m_forcedYaw = degrees * kRadiansPerDegree;
        WOWVR_INFO("Sound listener: forcing an extra %.1f deg of turn on the ears "
                   "(diagnostic; the picture does not follow it).", degrees);
    }

    int SoundListener::OnSetListener(const float* position, const float* forward,
                                     const float* up, const void* velocity)
    {
        ++m_totalCalls;

        SetListenerFn original = reinterpret_cast<SetListenerFn>(g_trampoline);
        if (original == nullptr)
        {
            return 0;
        }

        // Anything unexpected goes straight through. A listener update that is refused is
        // one frame of the client's own placement; a listener update that is corrupted is
        // a sound field that stays wrong until the next zone.
        if (!m_active || !Finite3(position) || !Finite3(forward) || !Finite3(up))
        {
            return original(position, forward, up, velocity);
        }

        const float length = sqrtf(forward[0] * forward[0] + forward[1] * forward[1]
                                   + forward[2] * forward[2]);
        if (!(length > 0.5f) || !(length < 2.0f))
        {
            return original(position, forward, up, velocity);
        }

        float sinPitch = forward[2] / length;
        if (sinPitch > 1.0f) { sinPitch = 1.0f; }
        if (sinPitch < -1.0f) { sinPitch = -1.0f; }

        const float yaw0 = atan2f(forward[1], forward[0]);
        const float pitch0 = asinf(sinPitch);

        const float deltaYaw = m_headYaw * m_yawSign + m_forcedYaw;
        const float deltaPitch = m_headPitch * m_pitchSign;

        float pitch1 = pitch0 + deltaPitch;
        if (pitch1 > kMaxPitch) { pitch1 = kMaxPitch; }
        if (pitch1 < -kMaxPitch) { pitch1 = -kMaxPitch; }

        // R = to * transpose(from): the rotation that carries the client's own listener
        // basis onto the one it would have if the camera were aimed where the head looks.
        // Applying it to the vectors we were handed, rather than rebuilding them from the
        // angles, keeps whatever else the client had put in them - the camera's own roll,
        // in the branch that reads the camera's up axis.
        float from[3][3];
        float to[3][3];
        BasisOf(yaw0, pitch0, from);
        BasisOf(yaw0 + deltaYaw, pitch1, to);

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

        float newForward[3];
        float newUp[3];
        Rotate(r, forward, newForward);
        Rotate(r, up, newUp);

        // Head roll, applied about the corrected forward axis. FMOD is given a forward
        // and an up vector, so a tilted head is representable exactly: tilt your head and
        // the stereo image tilts with it, which is what happens in a room.
        const float roll = m_headRoll * m_rollSign;
        if (fabsf(roll) > 1.0e-4f)
        {
            // left = forward x up, in this right-handed world. Rolling tips the up axis
            // towards one side and leaves the forward axis alone.
            float left[3];
            left[0] = newForward[1] * newUp[2] - newForward[2] * newUp[1];
            left[1] = newForward[2] * newUp[0] - newForward[0] * newUp[2];
            left[2] = newForward[0] * newUp[1] - newForward[1] * newUp[0];
            Normalise(left);

            const float c = cosf(roll);
            const float s = sinf(roll);
            for (int i = 0; i < 3; ++i)
            {
                newUp[i] = newUp[i] * c - left[i] * s;
            }
        }

        Normalise(newForward);
        Normalise(newUp);

        // Where the ears start from, before the head's own displacement.
        //
        // The client's base is the CHARACTER, not the camera: Sound_ListenerAtCharacter
        // defaults on, and its branch takes the position from the player object while
        // still taking the orientation from the camera. So the ears turned with the view
        // and never moved with it, and zooming a third-person camera out over a fire left
        // the fire exactly as loud. In a headset the viewpoint is the head, so the base
        // has to be the camera - everything else in this project already treats it as the
        // eye origin.
        float newPosition[3] = { position[0], position[1], position[2] };
        // Measured whether or not it is used, because the whole question this answers is
        // "how far from the viewpoint were the ears" - and reporting 0.0 for that while
        // the feature is switched off would say the opposite of the truth.
        float separation = 0.0f;
        if (m_camera != nullptr)
        {
            const float* cameraPosition =
                reinterpret_cast<const float*>(m_camera + kCameraPositionOffset);
            if (Finite3(cameraPosition))
            {
                const float dx = cameraPosition[0] - position[0];
                const float dy = cameraPosition[1] - position[1];
                const float dz = cameraPosition[2] - position[2];
                separation = sqrtf(dx * dx + dy * dy + dz * dz);

                if (m_earsAtCamera)
                {
                    newPosition[0] = cameraPosition[0];
                    newPosition[1] = cameraPosition[1];
                    newPosition[2] = cameraPosition[2];
                }
            }
        }
        m_lastBaseSeparation = separation;

        // The head's displacement, mapped onto the client's world axes.
        //
        // The offset arrives in the neutral (body) frame - right, up, forward in metres -
        // and that frame rides the game camera's heading, so the camera's own yaw is what
        // maps it into the world. Yaw only, and the vertical component onto world up:
        // real vertical motion spent along a pitched camera's up axis is the head-height
        // lock the projection path already had to fix once (projection_patch.cpp).
        Vec3 move;
        if (m_trackPosition)
        {
            const float cy = cosf(yaw0);
            const float sy = sinf(yaw0);
            const float scale = m_unitsPerMetre;

            // World forward is (cos yaw, sin yaw, 0) and world LEFT is 90 degrees on from
            // it, so right is the negation of that - the same convention the culling
            // rotation was measured against.
            const float right = m_headOffset.x * m_lateralSign;
            move.x = (cy * m_headOffset.z + sy * right) * scale;
            move.y = (sy * m_headOffset.z - cy * right) * scale;
            move.z = m_headOffset.y * scale;

            newPosition[0] += move.x;
            newPosition[1] += move.y;
            newPosition[2] += move.z;
        }

        if (m_dumpCalls > 0)
        {
            --m_dumpCalls;
            WOWVR_INFO("Sound listener: at (%.1f, %.1f, %.1f) facing (%.2f, %.2f, %.2f) "
                       "-> at (%.1f, %.1f, %.1f) facing (%.2f, %.2f, %.2f); head yaw "
                       "%+.1f pitch %+.1f roll %+.1f deg, move (%.2f, %.2f, %.2f) yards, "
                       "client's own base %.1f yards from the camera",
                       position[0], position[1], position[2],
                       forward[0], forward[1], forward[2],
                       newPosition[0], newPosition[1], newPosition[2],
                       newForward[0], newForward[1], newForward[2],
                       deltaYaw * kDegreesPerRadian, deltaPitch * kDegreesPerRadian,
                       roll * kDegreesPerRadian, move.x, move.y, move.z,
                       separation);
        }

        m_lastYawApplied = deltaYaw;
        m_lastPitchApplied = deltaPitch;
        m_lastMoveYards = move;
        m_lastPosition[0] = newPosition[0];
        m_lastPosition[1] = newPosition[1];
        m_lastPosition[2] = newPosition[2];
        m_lastForward[0] = newForward[0];
        m_lastForward[1] = newForward[1];
        m_lastForward[2] = newForward[2];
        ++m_totalCorrected;

        return original(newPosition, newForward, newUp, velocity);
    }

    void SoundListener::Report() const
    {
        WOWVR_INFO("Sound listener: %s, %s, ears at %s, %llu updates (%llu corrected), "
                   "last applied yaw %+.1f deg pitch %+.1f deg, displacement "
                   "(%.2f, %.2f, %.2f) yards; ears at (%.1f, %.1f, %.1f) facing "
                   "(%.2f, %.2f, %.2f), %.1f yards from where the client would have put "
                   "them",
                   m_patched ? "hooked" : "NOT hooked",
                   m_active ? "active" : "idle",
                   m_earsAtCamera ? (m_camera != nullptr ? "the camera"
                                                         : "the camera (NOT FOUND YET)")
                                  : "the client's own base",
                   static_cast<unsigned long long>(m_totalCalls),
                   static_cast<unsigned long long>(m_totalCorrected),
                   m_lastYawApplied * kDegreesPerRadian,
                   m_lastPitchApplied * kDegreesPerRadian,
                   m_lastMoveYards.x, m_lastMoveYards.y, m_lastMoveYards.z,
                   m_lastPosition[0], m_lastPosition[1], m_lastPosition[2],
                   m_lastForward[0], m_lastForward[1], m_lastForward[2],
                   m_lastBaseSeparation);

        // The client only updates its listener while the world is up. Zero here with the
        // hook in place is a login screen or a loading screen, not a fault - but zero
        // while the world is running means the choke point is not the one being used.
        if (m_patched && m_active && m_totalCalls == 0)
        {
            WOWVR_WARN("Sound listener: hooked but never called - the client has not "
                       "placed a listener at all.");
        }
    }

    SoundListener& HeadListener()
    {
        static SoundListener instance;
        return instance;
    }
}
