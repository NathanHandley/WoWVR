#include "game/comfort_vignette.h"

#include "core/log.h"
#include "game/game_camera.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace wowvr
{
    namespace
    {
        const uintptr_t kPublishedImageBase = 0x00400000u;
        // ClntObjMgrGetActivePlayerObj: no arguments, the local player's object or null.
        const uintptr_t kActivePlayerObjectRva = 0x004038F0u - kPublishedImageBase;
        typedef void*(__cdecl* ActivePlayerObjectFn)();
        const uintptr_t kUnitPosition = 0x798u;   // x, y, z
        const uintptr_t kUnitFacing = 0x7A8u;     // radians

        // What counts as the view moving without the body. Below these, nothing: idle
        // animation, a server position correction, the camera settling.
        const float kMoveYardsPerSecond = 0.75f;
        const float kTurnRadiansPerSecond = 0.35f;     // ~20 degrees a second
        const float kZoomYardsPerSecond = 1.0f;
        // More than this in one frame is a teleport or a loading screen, not motion.
        const float kJumpYards = 30.0f;
        // Keeps the vignette up briefly after motion stops, so it does not flicker
        // between steps.
        const float kHoldSeconds = 0.15f;
        const float kFadeInSeconds = 0.2f;
        const float kFadeOutSeconds = 0.35f;

        enum Reason { MovedReason = 1, TurnedReason = 2, CameraReason = 4, ZoomReason = 8 };

        void* ActivePlayer()
        {
            __try
            {
                return reinterpret_cast<ActivePlayerObjectFn>(
                    reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + kActivePlayerObjectRva)();
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return nullptr;
            }
        }

        bool ReadFloats(uintptr_t at, float* out, int count)
        {
            __try
            {
                memcpy(out, reinterpret_cast<const void*>(at), static_cast<size_t>(count) * sizeof(float));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        float AngleDelta(float a, float b)
        {
            float d = a - b;
            while (d > 3.14159265f) { d -= 6.28318531f; }
            while (d < -3.14159265f) { d += 6.28318531f; }
            return fabsf(d);
        }
    }

    void ComfortVignette::Update(float seconds, bool enabled)
    {
        if (!(seconds > 0.0f) || seconds > 0.5f)
        {
            seconds = 0.011f;
        }
        bool moving = false;

        // The player.
        float position[3] = {};
        float facing = 0.0f;
        void* player = ActivePlayer();
        const bool havePlayer = player != nullptr
            && ReadFloats(reinterpret_cast<uintptr_t>(player) + kUnitPosition, position, 3)
            && ReadFloats(reinterpret_cast<uintptr_t>(player) + kUnitFacing, &facing, 1)
            && std::isfinite(position[0]) && std::isfinite(position[1])
            && std::isfinite(position[2]) && std::isfinite(facing);

        // The camera without the head: its measured facing minus the free-look offset
        // WoWVR writes from the head, which the client adds on top of its own yaw.
        float cameraYaw = 0.0f;
        float cameraPitch = 0.0f;
        float orbit = 0.0f;
        bool haveCamera = false;
        if (GameCam().Update() && GameCam().CameraFacing(cameraYaw, cameraPitch))
        {
            cameraYaw -= GameCam().YawSign() * GameCam().FreeLookYawField();
            cameraPitch = GameCam().BasePitch();
            orbit = GameCam().OrbitRadius();
            haveCamera = std::isfinite(cameraYaw) && std::isfinite(cameraPitch) && std::isfinite(orbit);
        }

        if (havePlayer && haveCamera && m_havePrevious)
        {
            const float dx = position[0] - m_previousPosition[0];
            const float dy = position[1] - m_previousPosition[1];
            const float dz = position[2] - m_previousPosition[2];
            const float moved = sqrtf(dx * dx + dy * dy + dz * dz);
            if (moved < kJumpYards)
            {
                unsigned reasons = 0;
                if (moved / seconds > kMoveYardsPerSecond) { reasons |= MovedReason; }
                if (AngleDelta(facing, m_previousFacing) / seconds > kTurnRadiansPerSecond)
                {
                    reasons |= TurnedReason;
                }
                if ((AngleDelta(cameraYaw, m_previousCameraYaw)
                     + fabsf(cameraPitch - m_previousCameraPitch)) / seconds > kTurnRadiansPerSecond)
                {
                    reasons |= CameraReason;
                }
                if (fabsf(orbit - m_previousOrbit) / seconds > kZoomYardsPerSecond)
                {
                    reasons |= ZoomReason;
                }
                moving = reasons != 0;
                m_reasons |= reasons;
            }
        }
        m_havePrevious = havePlayer && haveCamera;
        if (m_havePrevious)
        {
            memcpy(m_previousPosition, position, sizeof(m_previousPosition));
            m_previousFacing = facing;
            m_previousCameraYaw = cameraYaw;
            m_previousCameraPitch = cameraPitch;
            m_previousOrbit = orbit;
        }

        ++m_framesSeen;
        if (moving)
        {
            ++m_framesMoving;
            m_holdSeconds = kHoldSeconds;
        }
        else if (m_holdSeconds > 0.0f)
        {
            m_holdSeconds -= seconds;
        }
        if (m_previewSeconds > 0.0f)
        {
            m_previewSeconds -= seconds;
        }

        const bool wanted = (enabled && (moving || m_holdSeconds > 0.0f)) || m_previewSeconds > 0.0f;
        if (wanted)
        {
            m_amount += seconds / kFadeInSeconds;
            if (m_amount > 1.0f) { m_amount = 1.0f; }
        }
        else
        {
            m_amount -= seconds / kFadeOutSeconds;
            if (m_amount < 0.0f) { m_amount = 0.0f; }
        }
    }

    void ComfortVignette::LogStatus()
    {
        WOWVR_INFO("Comfort vignette: on for %llu of %llu frames since the last report%s%s%s%s.",
                   m_framesMoving, m_framesSeen,
                   (m_reasons & MovedReason) ? "; moving" : "",
                   (m_reasons & TurnedReason) ? "; turning" : "",
                   (m_reasons & CameraReason) ? "; camera dragged" : "",
                   (m_reasons & ZoomReason) ? "; zooming" : "");
        m_framesMoving = 0;
        m_framesSeen = 0;
        m_reasons = 0;
    }

    ComfortVignette& Vignette()
    {
        static ComfortVignette instance;
        return instance;
    }
}
