#include "game/world_pointer.h"

#include "core/log.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace wowvr
{
    namespace
    {
        // Rebased against the running image, so a relocated client still works.
        const uintptr_t kPublishedImageBase = 0x00400000u;

        // Screen -> ray. Only caller is CGWorldFrame's 0x004F6450.
        const uintptr_t kScreenToRayRva = 0x004BF0F0u - kPublishedImageBase;
        const uint8_t kScreenToRayEntry[9] =
            { 0x55u, 0x8Bu, 0xECu, 0x81u, 0xECu, 0xF8u, 0x00u, 0x00u, 0x00u };
            // push ebp; mov ebp,esp; sub esp,0xF8

        // World -> screen, CGWorldFrame::GetScreenCoordinates.
        const uintptr_t kWorldToScreenRva = 0x004F6D20u - kPublishedImageBase;
        const uint8_t kWorldToScreenEntry[6] =
            { 0x55u, 0x8Bu, 0xECu, 0x83u, 0xECu, 0x24u };
            // push ebp; mov ebp,esp; sub esp,0x24

        const uintptr_t kWorldFramePtrRva = 0x00B7436Cu - kPublishedImageBase;

        // Width and height of the screen in the client's diagonal-normalised units
        // (width^2 + height^2 = 1), which is what GetScreenCoordinates hands back.
        // Scaled by 0x0047BFF0 on both the mouse and the projection side.
        const uintptr_t kDdcWidthRva = 0x00AC0CB4u - kPublishedImageBase;
        const uintptr_t kDdcHeightRva = 0x00AC0CB8u - kPublishedImageBase;

        // CGWorldFrame fields read by GetScreenCoordinates (see the disassembly from
        // 0x004F6E22): the projection rectangle the 0..1 result is scaled by, the
        // frame's own rectangle the on-screen test is made against, and its camera.
        const uintptr_t kFrameBottom = 0x64u;
        const uintptr_t kFrameLeft = 0x68u;
        const uintptr_t kFrameTop = 0x6Cu;
        const uintptr_t kFrameRight = 0x70u;
        const uintptr_t kProjRect0 = 0x330u;   // y extent is [0x338] - [0x330]
        const uintptr_t kProjRect1 = 0x334u;   // x extent is [0x33C] - [0x334]
        const uintptr_t kProjRect2 = 0x338u;
        const uintptr_t kProjRect3 = 0x33Cu;
        const uintptr_t kMouseRect = 0x320u;   // 0x004F6450's normalisation, logged only
        const uintptr_t kViewProjection = 0x340u;   // camera-relative view-projection, 4x4
        const uintptr_t kActiveCamera = 0x7E20u;

        // The camera object (CLIENT_INTERNALS.md section 2.1).
        const uintptr_t kCamPosition = 0x08u;
        const uintptr_t kCamForward = 0x14u;
        const uintptr_t kCamUp = 0x2Cu;
        const uintptr_t kCamNear = 0x38u;
        const uintptr_t kCamFar = 0x3Cu;
        const uintptr_t kCamLiveDistance = 0x118u;   // orbit distance to the target

        // CWorld::Intersect, the trace both the pick (0x004F99F8) and the camera's own
        // collision (0x00605D60) use: (start, end, hitPoint out, fraction in/out,
        // flags, 0), cdecl, true in al on a hit with the fraction shortened to it.
        const uintptr_t kWorldIntersectRva = 0x0077F310u - kPublishedImageBase;
        typedef unsigned char(__cdecl* WorldIntersectFn)(const float* start, const float* end,
                                                         float* hitPoint, float* fraction,
                                                         uint32_t flags, uint32_t unused);
        // The camera collision's own flags (0x00605E11).
        const uint32_t kCameraCollisionFlags = 0x00100171u;

        // The pick's own trace flags (0x004F9964) and where the trace leaves the GUID
        // of a game object it hit (read by the pick at 0x004F9A5C).
        const uint32_t kPickTraceFlags = 0x01000124u;
        const uintptr_t kTraceHitGuidRva = 0x00CD7768u - kPublishedImageBase;

        // Diagnostic: the pick trace itself (0x004F9930), thiscall (start*, end*, flags,
        // result*), ret 0x10; returns 0 for nothing, 2 for an object (GUID at result+0).
        const uintptr_t kPickTraceRva = 0x004F9930u - kPublishedImageBase;
        const uint8_t kPickTraceEntry[9] =
            { 0x55u, 0x8Bu, 0xECu, 0x81u, 0xECu, 0xC4u, 0x00u, 0x00u, 0x00u };
        const uintptr_t kMouseoverGuidRva = 0x00BD07A0u - kPublishedImageBase;
        typedef int(__fastcall* PickTraceFn)(uint8_t* worldFrame, void* unusedEdx,
                                             const float* start, const float* end,
                                             uint32_t flags, uint32_t* result);
        PickTraceFn g_pickTraceOriginal = nullptr;
        DWORD g_lastPickResultLog = 0;

        // Diagnostic: once a second, what the pick's world trace hits along the ray
        // WoWVR hands it and along the one the client would have used.
        DWORD g_lastPickLog = 0;


        typedef void(__cdecl* ScreenToRayFn)(float nx, float ny, float* start, float* end);

        // thiscall mirrored as fastcall with a dummy edx: the callee pops its three
        // stack arguments either way, which is what the client's 'ret 0xC' does. The
        // result is a full 32-bit value rather than bool: the client's flagless path
        // sets all of eax (mov eax, 1 / xor eax, eax), and a bool only promises al.
        typedef uint32_t(__fastcall* WorldToScreenFn)(uint8_t* worldFrame, void* unusedEdx,
                                                  const float* world, float* out,
                                                  uint32_t* flags);

        ScreenToRayFn g_screenToRayOriginal = nullptr;
        WorldToScreenFn g_worldToScreenOriginal = nullptr;
        bool g_installed = false;
        bool g_installFailed = false;

        struct State
        {
            bool active = false;
            pointing::Frame frame;
        };
        State g_state;

        // Diagnostics, all on the one thread.
        unsigned long long g_rayCalls = 0;
        unsigned long long g_projectCalls = 0;
        unsigned long long g_projectOnPanel = 0;
        unsigned long long g_rayStartsMoved = 0;
        unsigned long long g_basisFallbacks = 0;
        float g_lastRayU = -1.0f;
        float g_lastRayV = -1.0f;
        float g_lastRayOffAxisDegrees = 0.0f;
        bool g_frameLogged = false;

        uintptr_t ImageBase()
        {
            static uintptr_t base = 0;
            if (base == 0)
            {
                base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            }
            return base;
        }

        using pointing::CameraBasis;

        float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }

        Vec3 Cross(const Vec3& a, const Vec3& b)
        {
            Vec3 r;
            r.x = a.y * b.z - a.z * b.y;
            r.y = a.z * b.x - a.x * b.z;
            r.z = a.x * b.y - a.y * b.x;
            return r;
        }

        bool Finite(float value) { return value == value && std::fabs(value) < 1.0e30f; }

        // Copies raw floats out of client memory. Kept free of anything with a
        // destructor so __try is allowed here.
        bool CopyFloats(uintptr_t address, float* out, int count)
        {
            __try
            {
                memcpy(out, reinterpret_cast<const void*>(address),
                       static_cast<size_t>(count) * sizeof(float));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool ReadPointer(uintptr_t address, uintptr_t& out)
        {
            __try
            {
                out = *reinterpret_cast<const uintptr_t*>(address);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        uintptr_t CurrentWorldFrame()
        {
            uintptr_t frame = 0;
            if (!ReadPointer(ImageBase() + kWorldFramePtrRva, frame))
            {
                return 0;
            }
            return frame;
        }

        bool ReadCamera(uintptr_t worldFrame, CameraBasis& out)
        {
            uintptr_t camera = 0;
            if (worldFrame == 0 || !ReadPointer(worldFrame + kActiveCamera, camera)
                || camera == 0)
            {
                return false;
            }

            // +0x08 .. +0x3C in one read: position, forward, left, up, near, far.
            float raw[14] = {};
            if (!CopyFloats(camera + kCamPosition, raw, 14))
            {
                return false;
            }
            out.position = { raw[0], raw[1], raw[2] };
            out.nearPlane = raw[(kCamNear - kCamPosition) / 4u];
            out.farPlane = raw[(kCamFar - kCamPosition) / 4u];
            if (!Finite(out.position.x) || !Finite(out.position.y) || !Finite(out.position.z))
            {
                return false;
            }

            // The axes come from the world frame's camera-relative view-projection at
            // +0x340 - the matrix GetScreenCoordinates itself projects with, captured
            // by the same camera setup the pick runs - NOT from the camera object's own
            // basis at +0x14 / +0x2C. On a transport those are kept in the transport's
            // rotated frame (measured on the Kelethin lift: ~150 degrees apart from where
            // the client's own ray went), so a ray built from them missed everything
            // off the lift. Row-vector layout (0x004C2270: out[c] = sum v[r] * M[r][c]):
            // clip.w = view depth, so column 3 is the forward axis; columns 0 and 1 are
            // right and up scaled by the projection.
            float m[16] = {};
            if (CopyFloats(worldFrame + kViewProjection, m, 16))
            {
                Vec3 forward = { m[3], m[7], m[11] };
                Vec3 right = { m[0], m[4], m[8] };
                Vec3 up = { m[1], m[5], m[9] };
                const float fl = Length(forward);
                const float rl = Length(right);
                const float ul = Length(up);
                if (fl > 1.0e-4f && rl > 1.0e-4f && ul > 1.0e-4f && Finite(fl) && Finite(rl)
                    && Finite(ul))
                {
                    forward = { forward.x / fl, forward.y / fl, forward.z / fl };
                    right = { right.x / rl, right.y / rl, right.z / rl };
                    up = { up.x / ul, up.y / ul, up.z / ul };
                    if (std::fabs(Dot(forward, right)) < 0.05f && std::fabs(Dot(forward, up)) < 0.05f
                        && std::fabs(Dot(right, up)) < 0.05f)
                    {
                        out.forward = forward;
                        out.right = right;
                        out.up = up;
                        return true;
                    }
                }
            }

            // Fallback: the camera object's own basis (correct off transports).
            const int forwardIndex = static_cast<int>((kCamForward - kCamPosition) / 4u);
            const int upIndex = static_cast<int>((kCamUp - kCamPosition) / 4u);
            out.forward = { raw[forwardIndex], raw[forwardIndex + 1], raw[forwardIndex + 2] };
            out.up = { raw[upIndex], raw[upIndex + 1], raw[upIndex + 2] };
            // +0x20 is the LEFT axis (WoW is right-handed and Z-up); right is built as
            // forward x up instead of being read and negated (CLIENT_INTERNALS 2.3).
            out.right = Cross(out.forward, out.up);
            ++g_basisFallbacks;

            const float forwardLength = Length(out.forward);
            const float upLength = Length(out.up);
            const float rightLength = Length(out.right);
            if (std::fabs(forwardLength - 1.0f) > 0.05f || std::fabs(upLength - 1.0f) > 0.05f
                || std::fabs(rightLength - 1.0f) > 0.05f)
            {
                return false;
            }
            return true;
        }

        Vec3 ViewToWorldDirection(const CameraBasis& camera, const Vec3& view)
        {
            Vec3 world;
            world.x = camera.right.x * view.x + camera.up.x * view.y + camera.forward.x * view.z;
            world.y = camera.right.y * view.x + camera.up.y * view.y + camera.forward.y * view.z;
            world.z = camera.right.z * view.x + camera.up.z * view.y + camera.forward.z * view.z;
            return world;
        }

        Vec3 WorldToViewDirection(const CameraBasis& camera, const Vec3& world)
        {
            Vec3 view;
            view.x = Dot(world, camera.right);
            view.y = Dot(world, camera.up);
            view.z = Dot(world, camera.forward);
            return view;
        }

        void LogFrameOnce(uintptr_t worldFrame)
        {
            if (g_frameLogged)
            {
                return;
            }
            g_frameLogged = true;

            float frameRect[4] = {};
            float projRect[4] = {};
            float mouseRect[4] = {};
            float ddc[2] = {};
            if (CopyFloats(worldFrame + kFrameBottom, frameRect, 4)
                && CopyFloats(worldFrame + kProjRect0, projRect, 4)
                && CopyFloats(worldFrame + kMouseRect, mouseRect, 4)
                && CopyFloats(ImageBase() + kDdcWidthRva, ddc, 1)
                && CopyFloats(ImageBase() + kDdcHeightRva, ddc + 1, 1))
            {
                // Everything here should describe a world frame covering the whole
                // screen: frame and mouse rectangles spanning 0..ddc, projection
                // rectangle 0..1. Anything else means an addon has moved WorldFrame,
                // and the panel-to-screen mapping below assumes it has not.
                WOWVR_INFO("World pointing: screen %.4f x %.4f; world frame b/l/t/r "
                           "%.4f %.4f %.4f %.4f; projection rect %.4f %.4f %.4f %.4f; "
                           "mouse rect %.4f %.4f %.4f %.4f.", ddc[0], ddc[1],
                           frameRect[0], frameRect[1], frameRect[2], frameRect[3],
                           projRect[0], projRect[1], projRect[2], projRect[3],
                           mouseRect[0], mouseRect[1], mouseRect[2], mouseRect[3]);
            }
        }

        void LogTrace(const char* label, const float* from, const float* to)
        {
            float hit[3] = {};
            float fraction = 1.0f;
            const WorldIntersectFn intersect =
                reinterpret_cast<WorldIntersectFn>(ImageBase() + kWorldIntersectRva);
            const bool hitSomething = intersect(from, to, hit, &fraction, kPickTraceFlags, 0) != 0;
            uint32_t guid[2] = {};
            CopyFloats(ImageBase() + kTraceHitGuidRva, reinterpret_cast<float*>(guid), 2);
            const float dx = to[0] - from[0];
            const float dy = to[1] - from[1];
            const float dz = to[2] - from[2];
            const float length = sqrtf(dx * dx + dy * dy + dz * dz);
            WOWVR_INFO("Pick %s: from (%.2f %.2f %.2f) dir (%.3f %.3f %.3f) -> %s at %.2f yards, "
                       "object %08X%08X", label, from[0], from[1], from[2],
                       length > 0.0f ? dx / length : 0.0f, length > 0.0f ? dy / length : 0.0f,
                       length > 0.0f ? dz / length : 0.0f,
                       hitSomething ? "world hit" : "no world hit",
                       hitSomething ? fraction * length : length,
                       hitSomething ? guid[1] : 0u, hitSomething ? guid[0] : 0u);
        }

        // Entered in place of 0x004BF0F0. The client's own answer is computed first:
        // its near and far distances are kept so the trace keeps the reach the client
        // gave it, and only the direction and origin are replaced.
        void __cdecl ScreenToRayDetour(float nx, float ny, float* start, float* end)
        {
            g_screenToRayOriginal(nx, ny, start, end);

            if (!g_state.active || start == nullptr || end == nullptr)
            {
                return;
            }

            const uintptr_t worldFrame = CurrentWorldFrame();
            CameraBasis camera;
            if (!ReadCamera(worldFrame, camera))
            {
                return;
            }
            LogFrameOnce(worldFrame);

            // nx, ny are fractions of the world frame with y up; the panel texture is
            // the whole screen with v down.
            const float u = nx;
            const float v = 1.0f - ny;

            Vec3 originWorld;
            Vec3 directionWorld;
            if (!pointing::PanelToWorldRay(g_state.frame, camera, u, v, originWorld,
                                           directionWorld))
            {
                return;
            }

            // Where the pick may start. WoWVR turns the client's third-person camera
            // collision off in VR, so the camera can sit behind geometry the character
            // is standing inside - a lift's cage, a doorway - and a pick from there hits
            // that geometry first and never reaches what the pointer is on. On a monitor
            // the collision would have pulled the camera in front of it. So: trace from
            // the camera's target back to the camera exactly as that collision does, and
            // if something is in the way, start the pick at that depth along the ray.
            float startDistance = 0.0f;
            {
                uintptr_t cameraObject = 0;
                float distance = 0.0f;
                if (ReadPointer(worldFrame + kActiveCamera, cameraObject) && cameraObject != 0
                    && CopyFloats(cameraObject + kCamLiveDistance, &distance, 1)
                    && Finite(distance) && distance > 1.0f && distance < 100.0f)
                {
                    const Vec3 target = { camera.position.x + camera.forward.x * distance,
                                          camera.position.y + camera.forward.y * distance,
                                          camera.position.z + camera.forward.z * distance };
                    const float eye[3] = { camera.position.x + originWorld.x,
                                           camera.position.y + originWorld.y,
                                           camera.position.z + originWorld.z };
                    const float from[3] = { target.x, target.y, target.z };
                    float hit[3] = {};
                    float fraction = 1.0f;
                    const WorldIntersectFn intersect =
                        reinterpret_cast<WorldIntersectFn>(ImageBase() + kWorldIntersectRva);
                    if (intersect(from, eye, hit, &fraction, kCameraCollisionFlags, 0) != 0
                        && fraction >= 0.0f && fraction < 1.0f)
                    {
                        // The blocking point, relative to the eye, measured along the ray.
                        const Vec3 blocked = {
                            target.x + (eye[0] - target.x) * fraction - eye[0],
                            target.y + (eye[1] - target.y) * fraction - eye[1],
                            target.z + (eye[2] - target.z) * fraction - eye[2] };
                        startDistance = Dot(blocked, directionWorld);
                        ++g_rayStartsMoved;
                    }
                }
            }

            const Vec3 originalStart = { start[0], start[1], start[2] };
            const Vec3 originalEnd = { end[0], end[1], end[2] };
            float nearDistance = Length(originalStart);
            float farDistance = Length(originalEnd);
            if (!Finite(nearDistance) || !Finite(farDistance) || !(farDistance > nearDistance))
            {
                // The client refused the input (it does outside 0..1) and wrote nothing.
                nearDistance = camera.nearPlane;
                farDistance = camera.farPlane;
            }
            if (!(farDistance > 0.0f))
            {
                return;
            }
            if (startDistance > nearDistance && startDistance < farDistance)
            {
                nearDistance = startDistance;
            }

            const DWORD now = GetTickCount();
            if (now - g_lastPickLog > 1000u)
            {
                g_lastPickLog = now;
                const float clientFrom[3] = { camera.position.x + originalStart.x,
                                              camera.position.y + originalStart.y,
                                              camera.position.z + originalStart.z };
                const float clientTo[3] = { camera.position.x + originalEnd.x,
                                            camera.position.y + originalEnd.y,
                                            camera.position.z + originalEnd.z };
                const float ourFrom[3] = {
                    camera.position.x + originWorld.x + directionWorld.x * nearDistance,
                    camera.position.y + originWorld.y + directionWorld.y * nearDistance,
                    camera.position.z + originWorld.z + directionWorld.z * nearDistance };
                const float ourTo[3] = {
                    camera.position.x + originWorld.x + directionWorld.x * farDistance,
                    camera.position.y + originWorld.y + directionWorld.y * farDistance,
                    camera.position.z + originWorld.z + directionWorld.z * farDistance };
                WOWVR_INFO("Pick: camera (%.2f %.2f %.2f) forward (%.3f %.3f %.3f), live distance "
                           "moved start %s.", camera.position.x, camera.position.y,
                           camera.position.z, camera.forward.x, camera.forward.y,
                           camera.forward.z, startDistance > 0.0f ? "yes" : "no");
                LogTrace("as WoWVR aims it", ourFrom, ourTo);
                LogTrace("as the client aimed it", clientFrom, clientTo);
            }

            start[0] = originWorld.x + directionWorld.x * nearDistance;
            start[1] = originWorld.y + directionWorld.y * nearDistance;
            start[2] = originWorld.z + directionWorld.z * nearDistance;
            end[0] = originWorld.x + directionWorld.x * farDistance;
            end[1] = originWorld.y + directionWorld.y * farDistance;
            end[2] = originWorld.z + directionWorld.z * farDistance;

            ++g_rayCalls;
            g_lastRayU = u;
            g_lastRayV = v;
            const float along = Dot(directionWorld, camera.forward);
            g_lastRayOffAxisDegrees =
                std::acos(along > 1.0f ? 1.0f : (along < -1.0f ? -1.0f : along)) * 57.29578f;
        }

        // Entered in place of GetScreenCoordinates. Replicates the client's contract
        // exactly - output in screen units relative to the frame, depth in out[2], the
        // four on-screen bits in *flags and the return value - but measures it on the
        // panel, from the head.
        uint32_t __fastcall WorldToScreenDetour(uint8_t* worldFrame, void* unusedEdx,
                                            const float* world, float* out, uint32_t* flags)
        {
            if (!g_state.active || worldFrame == nullptr || world == nullptr || out == nullptr)
            {
                return g_worldToScreenOriginal(worldFrame, unusedEdx, world, out, flags);
            }

            const uintptr_t frame = reinterpret_cast<uintptr_t>(worldFrame);
            CameraBasis camera;
            float frameRect[4] = {};     // bottom, left, top, right
            float projRect[4] = {};
            float ddc[2] = {};
            float point[3] = {};
            if (!ReadCamera(frame, camera)
                || !CopyFloats(frame + kFrameBottom, frameRect, 4)
                || !CopyFloats(frame + kProjRect0, projRect, 4)
                || !CopyFloats(ImageBase() + kDdcWidthRva, ddc, 1)
                || !CopyFloats(ImageBase() + kDdcHeightRva, ddc + 1, 1)
                || !CopyFloats(reinterpret_cast<uintptr_t>(world), point, 3))
            {
                return g_worldToScreenOriginal(worldFrame, unusedEdx, world, out, flags);
            }

            ++g_projectCalls;

            const Vec3 target = { point[0], point[1], point[2] };
            float u = 0.0f;
            float v = 0.0f;
            float distanceYards = 0.0f;
            if (!pointing::WorldToPanel(g_state.frame, camera, target, u, v, distanceYards))
            {
                // The client's own early out (behind the near plane) writes nothing and
                // returns false; this is the same answer for "nowhere on the panel".
                return 0u;
            }

            // The panel IS the screen: u across it, v down it. The client's scaling
            // from there on is reproduced as-is (0x004F6E22 onwards).
            const float x01 = u;
            const float y01 = 1.0f - v;
            float x = ddc[0] * x01 * (projRect[3] - projRect[1]);
            float y = ddc[1] * y01 * (projRect[2] - projRect[0]);
            if (frameRect[1] < 0.0f) { x -= frameRect[1]; }
            if (frameRect[0] < 0.0f) { y -= frameRect[0]; }

            out[0] = x;
            out[1] = y;
            // Depth: the client hands back the camera-space depth here. Distance from
            // the head along the line actually looked down is the same quantity for a
            // headset, and stays meaningful for things beside or behind the camera.
            out[2] = distanceYards;

            const float width = frameRect[3] - frameRect[1];
            const float height = frameRect[2] - frameRect[0];
            const uint32_t bits = (x >= 0.0f ? 1u : 0u)
                                | (y >= 0.0f ? 2u : 0u)
                                | (x <= width ? 4u : 0u)
                                | (y <= height ? 8u : 0u);
            if (flags != nullptr)
            {
                *flags = bits;
            }
            if (bits == 0x0Fu)
            {
                ++g_projectOnPanel;
                return 1u;
            }
            return 0u;
        }

        int __fastcall PickTraceDetour(uint8_t* worldFrame, void* unusedEdx, const float* start,
                                       const float* end, uint32_t flags, uint32_t* result)
        {
            const int kind = g_pickTraceOriginal(worldFrame, unusedEdx, start, end, flags, result);
            const DWORD now = GetTickCount();
            if (g_state.active && now - g_lastPickResultLog > 1000u)
            {
                g_lastPickResultLog = now;
                uint32_t mouseover[2] = {};
                CopyFloats(ImageBase() + kMouseoverGuidRva, reinterpret_cast<float*>(mouseover), 2);
                const float length = (start != nullptr && end != nullptr)
                    ? sqrtf((end[0] - start[0]) * (end[0] - start[0])
                            + (end[1] - start[1]) * (end[1] - start[1])
                            + (end[2] - start[2]) * (end[2] - start[2]))
                    : 0.0f;
                WOWVR_INFO("Pick result: flags 0x%08X, %.1f-yard ray -> kind %d, object %08X%08X; "
                           "mouseover %08X%08X", flags, length, kind,
                           (kind != 0 && result != nullptr) ? result[1] : 0u,
                           (kind != 0 && result != nullptr) ? result[0] : 0u,
                           mouseover[1], mouseover[0]);
            }
            return kind;
        }

        bool InstallDetour(uintptr_t rva, const uint8_t* expected, size_t length,
                           const void* detour, void** trampolineOut, const char* name)
        {
            uint8_t* site = reinterpret_cast<uint8_t*>(ImageBase() + rva);

            uint8_t patched[16] = {};
            patched[0] = 0xE9u;
            const int32_t rel = static_cast<int32_t>(
                reinterpret_cast<uintptr_t>(detour) - (reinterpret_cast<uintptr_t>(site) + 5));
            memcpy(patched + 1, &rel, 4);
            for (size_t i = 5; i < length; ++i)
            {
                patched[i] = 0x90u;
            }

            if (memcmp(site, patched, length) == 0)
            {
                return *trampolineOut != nullptr;
            }
            if (memcmp(site, expected, length) != 0)
            {
                WOWVR_WARN("World pointing: %s at 0x%08X does not hold the expected bytes; "
                           "leaving the client's own version in place.", name,
                           static_cast<unsigned>(reinterpret_cast<uintptr_t>(site)));
                return false;
            }

            // Trampoline: the displaced whole instructions, then a jump back to the
            // first byte after them. Never freed - the detour may be entered for as
            // long as the process lives.
            uint8_t* stub = static_cast<uint8_t*>(
                VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            if (stub == nullptr)
            {
                WOWVR_WARN("World pointing: could not allocate the %s trampoline.", name);
                return false;
            }
            size_t n = 0;
            memcpy(stub, expected, length);
            n += length;
            stub[n++] = 0xE9u;
            const uintptr_t resume = reinterpret_cast<uintptr_t>(site) + length;
            const int32_t back = static_cast<int32_t>(
                resume - (reinterpret_cast<uintptr_t>(stub) + n + 4));
            memcpy(stub + n, &back, 4);
            n += 4;
            FlushInstructionCache(GetCurrentProcess(), stub, n);

            // Published before the jump goes in, so the detour can never run without
            // somewhere to hand the call on to.
            *trampolineOut = stub;

            DWORD previous = 0;
            if (!VirtualProtect(site, length, PAGE_EXECUTE_READWRITE, &previous))
            {
                WOWVR_WARN("World pointing: %s at 0x%08X could not be made writable.", name,
                           static_cast<unsigned>(reinterpret_cast<uintptr_t>(site)));
                return false;
            }
            memcpy(site, patched, length);
            VirtualProtect(site, length, previous, &previous);
            FlushInstructionCache(GetCurrentProcess(), site, length);
            WOWVR_INFO("World pointing: %s hooked at 0x%08X.", name,
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(site)));
            return true;
        }
    }

    bool WorldPointer::Install()
    {
        if (g_installed)
        {
            return true;
        }
        if (g_installFailed)
        {
            return false;
        }

        // Both or neither: one without the other would make the pointer and the
        // nameplates disagree, which is worse than the client's consistent mistake.
        const uint8_t* rayCode = reinterpret_cast<const uint8_t*>(ImageBase() + kScreenToRayRva);
        const uint8_t* projectCode =
            reinterpret_cast<const uint8_t*>(ImageBase() + kWorldToScreenRva);
        if (memcmp(rayCode, kScreenToRayEntry, sizeof(kScreenToRayEntry)) != 0
            || memcmp(projectCode, kWorldToScreenEntry, sizeof(kWorldToScreenEntry)) != 0)
        {
            g_installFailed = true;
            WOWVR_WARN("World pointing: the client's pick and projection entry points are "
                       "not the 3.3.5a (12340) code this was written against; left alone.");
            return false;
        }

        void* rayTrampoline = nullptr;
        void* projectTrampoline = nullptr;
        // If either fails, whatever did go in stays inert: Update only ever activates
        // the pair together.
        const bool projectOk = InstallDetour(kWorldToScreenRva, kWorldToScreenEntry,
                                             sizeof(kWorldToScreenEntry),
                                             reinterpret_cast<const void*>(&WorldToScreenDetour),
                                             &projectTrampoline, "GetScreenCoordinates");
        if (projectOk)
        {
            g_worldToScreenOriginal = reinterpret_cast<WorldToScreenFn>(projectTrampoline);
        }
        const bool rayOk = projectOk
            && InstallDetour(kScreenToRayRva, kScreenToRayEntry, sizeof(kScreenToRayEntry),
                             reinterpret_cast<const void*>(&ScreenToRayDetour),
                             &rayTrampoline, "screen-to-ray");
        if (rayOk)
        {
            g_screenToRayOriginal = reinterpret_cast<ScreenToRayFn>(rayTrampoline);
        }

        // Diagnostic only; failure to install it changes nothing.
        void* pickTrampoline = nullptr;
        if (rayOk && InstallDetour(kPickTraceRva, kPickTraceEntry, sizeof(kPickTraceEntry),
                                   reinterpret_cast<const void*>(&PickTraceDetour),
                                   &pickTrampoline, "pick trace (diagnostic)"))
        {
            g_pickTraceOriginal = reinterpret_cast<PickTraceFn>(pickTrampoline);
        }

        if (!projectOk || !rayOk)
        {
            g_installFailed = true;
            return false;
        }

        g_installed = true;
        return true;
    }

    void WorldPointer::Update(bool active, const PanelShape& shape, const Vec3& headBody,
                              const Mat4& gameViewToBody, float unitsPerMetre)
    {
        g_state.frame.shape = shape;
        g_state.frame.headBody = headBody;
        g_state.frame.gameViewToBody = gameViewToBody;
        g_state.frame.bodyToGameView = Mat4Transpose(gameViewToBody);
        g_state.frame.unitsPerMetre = (unitsPerMetre > 0.0f) ? unitsPerMetre : 1.0936f;
        g_state.active = active && g_installed && g_screenToRayOriginal != nullptr
                         && g_worldToScreenOriginal != nullptr;
    }

    void WorldPointer::Deactivate()
    {
        g_state.active = false;
    }

    void WorldPointer::LogStatus() const
    {
        WOWVR_INFO("World pointing: %s; %llu pointer rays (last at u %.3f v %.3f, %.1f deg "
                   "off the camera's axis, %llu started past geometry behind the character), "
                   "%llu projections, %llu on the panel, %llu on the camera-object fallback "
                   "basis.",
                   g_installed ? (g_state.active ? "active" : "installed, idle")
                               : (g_installFailed ? "not installed" : "pending"),
                   g_rayCalls, g_lastRayU, g_lastRayV, g_lastRayOffAxisDegrees,
                   g_rayStartsMoved, g_projectCalls, g_projectOnPanel, g_basisFallbacks);
        g_rayStartsMoved = 0;
        g_basisFallbacks = 0;
        g_rayCalls = 0;
        g_projectCalls = 0;
        g_projectOnPanel = 0;
    }

    namespace pointing
    {
        bool PanelToWorldRay(const Frame& frame, const CameraBasis& camera, float u, float v,
                             Vec3& originRelative, Vec3& direction)
        {
            const Vec3 onPanel = frame.shape.BodyPoint(u, v);
            Vec3 body = { onPanel.x - frame.headBody.x, onPanel.y - frame.headBody.y,
                          onPanel.z - frame.headBody.z };
            const float length = Length(body);
            if (!(length > 1.0e-4f))
            {
                return false;
            }
            body.x /= length;
            body.y /= length;
            body.z /= length;

            // Body metres -> game view yards -> world. The head sits at headBody in the
            // body frame, whose origin is the camera (the neutral head position).
            const float upm = frame.unitsPerMetre;
            const Vec3 headYards = { frame.headBody.x * upm, frame.headBody.y * upm,
                                     frame.headBody.z * upm };
            originRelative = ViewToWorldDirection(
                camera, Mat4TransformDirection(headYards, frame.bodyToGameView));
            direction = ViewToWorldDirection(
                camera, Mat4TransformDirection(body, frame.bodyToGameView));
            return true;
        }

        bool WorldToPanel(const Frame& frame, const CameraBasis& camera, const Vec3& world,
                          float& u, float& v, float& distanceYards)
        {
            // The point in the camera's view space, then the body frame, in metres.
            const Vec3 relative = { world.x - camera.position.x, world.y - camera.position.y,
                                    world.z - camera.position.z };
            const Vec3 viewYards = WorldToViewDirection(camera, relative);
            const Vec3 bodyYards = Mat4TransformDirection(viewYards, frame.gameViewToBody);
            const float upm = frame.unitsPerMetre;
            const Vec3 body = { bodyYards.x / upm, bodyYards.y / upm, bodyYards.z / upm };

            const Vec3 fromHead = { body.x - frame.headBody.x, body.y - frame.headBody.y,
                                    body.z - frame.headBody.z };
            const float distanceMetres = Length(fromHead);
            if (!(distanceMetres > 1.0e-4f))
            {
                return false;
            }
            if (!frame.shape.IntersectBody(frame.headBody, fromHead, u, v))
            {
                return false;
            }
            distanceYards = distanceMetres * upm;
            return true;
        }
    }

    WorldPointer& Pointer()
    {
        static WorldPointer instance;
        return instance;
    }
}
