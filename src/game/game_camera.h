#pragma once

#include "core/math3d.h"

#include <cstdint>

namespace wowvr
{
    // WoW's camera, reached the way the client itself reaches it.
    //
    // Every previous attempt at this searched memory: sweep the address space for
    // something camera-shaped, nudge it, keep whatever moved. That works often enough to
    // be encouraging and never often enough to ship - the object is allocated afresh each
    // launch, hundreds of thousands of places match any layout you can describe, and a
    // wrong guess is written into somebody else's data.
    //
    // None of that is necessary. The client exposes FlipCameraYaw(degrees) to Lua - the
    // "look behind" macro - and its implementation says exactly where the camera is and
    // which field to write:
    //
    //     0x005FF2FA  call 0x004F5960              ; the active camera, into EAX
    //     0x005FF302  fmul [0x009E2B40]            ; degrees * 0.0174533
    //     0x005FF309  fadd dword ptr [eax + 0x12C] ; += camera->freeLookYaw
    //     0x005FF30F  fstp dword ptr [eax + 0x12C]
    //
    //     0x004F5960  mov eax, [0x00B7436C]        ; the world frame
    //     0x004F5965  test eax, eax
    //     0x004F5969  mov eax, [eax + 0x7E20]      ; its active camera
    //
    // So the camera is *(*(0x00B7436C) + 0x7E20), and +0x12C is a yaw the client ADDS to
    // whatever heading the camera would otherwise have. That last part is what makes this
    // the right lever rather than merely a working one: it is an offset, not an absolute
    // heading, so writing it turns the view without touching the character - which is the
    // whole feature. FlipCameraYaw(180) is the client's own use of it.
    //
    // The matching pitch offset is +0x134, added to the camera's pitch and then clamped to
    // +/-1.55334 rad (89 degrees) at 0x006045E9.
    //
    // Both offsets are skipped while a smoothed transition is running, which the client
    // signals with a positive frame counter at +0xAC (yaw) and +0xB0 (pitch):
    //
    //     0x0060459D  cmp dword ptr [esi + 0xAC], 0   ; >0 -> skip the yaw offset
    //     0x006045C1  cmp dword ptr [esi + 0xB0], 0   ; >0 -> skip the pitch offset
    //
    // Addresses are rebased against wherever the image actually loaded, so this survives a
    // relocated client; it does not survive a different client build, which is why the
    // object is sanity-checked before anything is written to it.
    class GameCamera
    {
    public:
        // Re-resolves the pointer chain. Cheap - two dereferences and a validity check -
        // and called every frame, because the camera object is destroyed and rebuilt
        // across loading screens.
        bool Update();

        bool Found() const { return m_camera != nullptr; }
        uintptr_t Address() const { return reinterpret_cast<uintptr_t>(m_camera); }

        // Turns the view without turning the character. Absolute, not incremental: the
        // client owns these fields between our writes and may smooth them back toward
        // zero, so each frame states the whole intent rather than a delta.
        void SetFreeLook(float yawRadians, float pitchRadians);
        void ClearFreeLook();

        // Which way round the client's two offset fields run, relative to the frame the
        // head pose is expressed in.
        //
        // Yaw is +1: with the pose scalar previously negated, the view came out correct
        // while the culling went the wrong way, which only works out if the client turned
        // by exactly what was written. Pitch is -1: the pose scalar was NOT negated, agreed
        // with the head matrix, and the world still moved opposite to the head - so the
        // inversion has to be the client's.
        //
        // Exposed rather than baked in because a sign is the one thing worth being able to
        // flip in the field without a rebuild.
        void SetSigns(float yawSign, float pitchSign);
        float YawSign() const { return m_yawSign; }
        float PitchSign() const { return m_pitchSign; }

        // What was actually written, which is what the projection substitution has to take
        // back out so the head rotation does not land twice. Zero whenever the camera is
        // not under our control, so the caller needs no separate "is it on" test.
        float AppliedYaw() const { return m_appliedYaw; }
        float AppliedPitch() const { return m_appliedPitch; }

        // What the client is holding right now, read back rather than remembered - the
        // client owns these fields between our writes and may have moved them.
        float FreeLookYawField() const;
        float FreeLookPitchField() const;

        // Where the camera is ACTUALLY pointing, from its own orientation matrix at +0x14
        // rather than from the offsets we asked for.
        //
        // The difference between the two is the only way to see what the client did with a
        // request: it clamps pitch, and it can refuse an offset outright while a smoothed
        // transition is running. Everything else here infers the client's behaviour from
        // pixels, which fails completely when the view is filled with featureless snow.
        //
        // WoW is Z-up, so yaw is atan2(y, x) of the forward vector and pitch is asin(z).
        bool CameraFacing(float& yawRadians, float& pitchRadians) const;

        // The camera's own pitch before our offset, and the limit field the client clamps
        // the total against.
        float BasePitch() const;
        float PitchLimitField() const;

        // The frustum the client culls, streams and sizes its shadow cascades against,
        // held inside the camera object at +0x40 (field of view) and +0x44 (aspect).
        //
        // This is the copy the renderer reads. The global at 0x00ABFC38 is only the
        // setting: writing it has always changed nothing on screen, because the client
        // copies it in here once a frame and then reads it from here. Widening these two
        // culls wider than the window is actually showing, which is the alternative to
        // running the client at an aspect ratio that makes its login screen unusable.
        //
        // 0 for either leaves that one alone.
        void SetCullFrustum(float fovRadians, float aspect);
        float CullFov() const;
        float CullAspect() const;

        // Multiplies the client's own field of view, so the frustum it culls against is
        // wider than the one we display.
        //
        // Measured, not assumed: writing 0.8 rad in place of the client's 1.5708 visibly
        // removed the tree at the right edge and the mountain slope at the left, and
        // writing 2.4 rad rendered identically to the default with nothing distorted. The
        // client does not recompute this field, so the value stays where it is put - which
        // is also why the ORIGINAL has to be remembered rather than the current value
        // scaled again each frame, or the widening compounds away to nothing useful.
        //
        // The margin matters because the camera is written at the end of one frame and
        // culled against during the next: without it, a fast head turn outruns the frustum
        // and the geometry it should have revealed has not been drawn. It also covers head
        // roll, which is not applied to the game camera at all.
        //
        // 1.0 restores the client's own value.
        void SetCullWiden(float scale);

        // How far aiming the camera has MOVED it, in its own view frame, in yards.
        //
        // In third person the camera rides a sphere around the character, so pointing it at
        // the head displaces it - up into the sky when you look down, out to the side when
        // you turn. Predicting that from a radius and an angle does not work, because the
        // radius is not a constant: the client pulls the camera in whenever it would clip
        // terrain, and it was measured swinging between 0.82 and 5.27 yards during a single
        // pass of head movement.
        //
        // So this measures instead of predicting. The camera's own position, basis and live
        // radius are all right there in the object; the head-neutral direction is the current
        // yaw minus the yaw offset we applied, at level pitch; and the displacement is the
        // radius times the difference between the two forward vectors, resolved onto the
        // camera's own axes.
        //
        // The model itself is confirmed rather than assumed: two camera positions sampled at
        // head pitch +0.8 and -0.8 differed by (-0.003, 0.005, -22.947) yards against a
        // predicted (-0.003, 0.005, -22.947).
        //
        // Only the yaw offset is passed, because only yaw IS an offset; see the comment on
        // the reference direction in the implementation.
        bool OrbitDisplacementView(float appliedYaw, Vec3& out) const;

        // Holds the live camera distance at its setting, defeating the client's camera
        // collision.
        //
        // Superseded by SetCollision and kept only as a diagnostic. It does not work: the
        // client recomputes the distance after this runs, so the value is gone by the time
        // anything is drawn with it. Measured holding at 12.00 through a head yaw and being
        // overwritten straight back down to 0.79 through a head pitch.
        void PinOrbitRadius(bool on);

        // Switches off the client's camera collision, in the client's own code.
        //
        // Collision is what moves a third-person camera about: pitching the view sweeps the
        // camera through the ground behind the character and the client hauls it in to keep
        // it from clipping, which reads as "nose up zooms in, nose down zooms out". Measured
        // collapsing from 15.0 to 0.79 yards across one head pitch. No correction applied to
        // the ORBIT can cancel that, because it is not an orbit - the camera is somewhere
        // else entirely.
        //
        // A data breakpoint on the live distance named the two instructions that write it -
        // 0x006076FE and 0x00603F6F, and nothing at all while the head is still - and the
        // first takes its value from a local that 0x00605D60 fills in by reference:
        //
        //     0x006074CB  lea eax, [ebp - 8]      ; &distance
        //     0x006074CE  push eax
        //     0x006074D5  call 0x00605D60         ; the collision test, and its only caller
        //
        // That function already contains the branch this needs. Its first act is to decide
        // whether to collide at all:
        //
        //     0x00605DCA  test dl, 8              ; camera->flags & 8
        //     0x00605DCD  je 0x00605DE5           ; -> the world trace, flags 0x00100171
        //     0x00605DCF  ...                     ; fall through: return the distance as-is
        //
        // Note which way round that runs: the JUMP goes to the collision code and the FALL
        // THROUGH is the early return. It reads backwards, and taking it at face value cost
        // a measurement - rewriting the jump as unconditional makes collision permanent, and
        // since the flag was already set it changed precisely nothing, which is a very
        // convincing way to look like the wrong site had been found.
        //
        // So the patch removes the jump rather than forcing it: 74 16 -> 90 90, at a site
        // with exactly one caller. Zoom is untouched, because the mouse wheel drives a
        // separate interpolation at 0x00603F6F toward the target distance and this path
        // hands that interpolated value straight back rather than replacing it.
        //
        // Verified against the original byte before writing, so a different client build
        // is declined rather than corrupted.
        bool SetCollision(bool enabled);
        bool CollisionPatched() const { return m_collisionPatched; }

        // The camera's position in the world, and its forward and up axes.
        //
        // Ground truth for the orbit correction, which is otherwise a chain of conventions -
        // which way the client's pitch field runs, whether +0x20 is the right vector or the
        // left one - each of which can be argued either way and two of which were wrong.
        // Sampling the position with the camera aimed and again with it not needs none of
        // them: the difference IS the displacement.
        //
        // +0x08 is where the client stores it, from 0x006075CD, which writes the result of
        // the camera-position calculation to [esi + 8] three floats at a time.
        bool CameraPose(Vec3& position, Vec3& forward, Vec3& up) const;

        // Distance from the character in yards, or 0 in first person. Turning the yaw
        // offset swings the camera around the character rather than pivoting it in place,
        // so this is the radius of that swing.
        float OrbitRadius() const;

        // One-time report of the object's contents, cross-referenced against the near
        // plane, far plane and field of view already decoded from the projection matrix on
        // the wire. Those three are known independently, so finding them inside this object
        // is proof it is the camera rather than a hypothesis about it - and it locates the
        // field-of-view copy the renderer reads, which no search has ever found.
        void DumpObject(float nearPlane, float farPlane, float halfFovRadians);

    private:
        bool Plausible(const uint8_t* camera) const;

        uint8_t* m_camera = nullptr;
        float m_appliedYaw = 0.0f;
        float m_appliedPitch = 0.0f;
        float m_yawSign = 1.0f;
        float m_pitchSign = -1.0f;
        bool m_collisionPatched = false;
        float m_baseCullFov = 0.0f;
        float m_widenScale = 0.0f;
        bool m_widenLogged = false;
        bool m_announced = false;
        bool m_dumped = false;
        int m_suppressedFrames = 0;
        int m_failures = 0;
    };

    GameCamera& GameCam();
}
