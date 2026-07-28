#pragma once

#include "core/math3d.h"

#include <cstdint>

namespace wowvr
{
    // Put the FMOD listener on the HEAD, not on the character and not on the camera.
    //
    // The client places its listener from the camera - optionally pulled back and lifted
    // by Sound_ListenerBackDist and Sound_ListenerUpDist, or parked on the character when
    // Sound_ListenerAtCharacter is set - and orients it along the camera's forward axis.
    // Worn, that is wrong in both halves: turning your head does not move the stereo
    // image at all, and stepping towards something in the room does not bring it closer.
    // The eyes have followed the head since phase 3; the ears never left the character.
    //
    // The client's own code, from a linear sweep of Wow.exe:
    //
    //   0x004FA740  the world update's sound pass. Reads the camera at [world + 0x7E20],
    //               applies Sound_ListenerBackDist / Sound_ListenerUpDist (the CVar
    //               pointers it caches at 0x00B743B4 and 0x00B743B0), and calls the
    //               setter below from one of four branches - listener at character,
    //               at camera, or at camera pulled back, plus a fallback.
    //   0x004C5B20  SetListenerAttributes(const C3Vector* position,
    //                                     const C3Vector* forward,
    //                                     const C3Vector* up,
    //                                     const C3Vector* velocity)   cdecl
    //               A four-argument wrapper with exactly those four call sites, all in
    //               the function above. Forwards to 0x00879320.
    //   0x00879320  guards on the sound system being up ([0x00D43814]) and calls
    //               0x008D14C0 with (system at [0x00D43800], listener 0, position,
    //               velocity, forward, up) - FMOD's Set3DListenerAttributes, in that
    //               exact argument order. Velocity is always null, so there is no
    //               doppler on the listener to disturb.
    //
    // Hooking the wrapper is what makes this safe: it is the single choke point every
    // branch of the client's own listener logic funnels through, so whichever placement
    // rule the player's CVars select, the head correction lands on top of it rather than
    // replacing it. Whatever the client decided the base position and heading should be,
    // this turns that heading by the head's rotation and walks that position by the
    // head's displacement - the same two quantities the eye projection already applies.
    //
    // Nothing is written into the client. The four vectors arrive as pointers to the
    // caller's stack (and, for one branch, to a client global holding world up), and the
    // corrected copies go into storage of our own, so the client's own values are never
    // modified - only what the wrapper is handed.
    class SoundListener
    {
    public:
        // Detours 0x004C5B20. Verifies the six bytes it replaces first, so a client this
        // was not decoded from is declined rather than corrupted.
        bool Enable(bool on);
        bool Enabled() const { return m_patched; }

        // False passes every call straight through untouched, which is the flat client's
        // behaviour. The detour stays in place; it just stops doing anything.
        void SetActive(bool active) { m_active = active; }
        bool Active() const { return m_active; }

        // Pushed once a frame from the Present hook, because the head pose can only be
        // sampled there. Everything else this needs arrives as an argument.
        void SetHeadRotation(float yawRadians, float pitchRadians, float rollRadians);

        // Displacement since recentring, in metres, in the NEUTRAL (body) frame:
        // x right, y up, z forward. That frame rides the game camera's heading, which is
        // why it can be mapped onto the camera's world axes below. The head-frame copy
        // would turn with the head and count the head's rotation a second time.
        void SetHeadOffsetMetres(const Vec3& metres);

        // Game units per metre, so a real step of a metre moves the listener the same
        // distance the eyes were moved by it. Shared with the projection path.
        void SetUnitsPerMetre(float units);

        // The camera object, read for the viewpoint the ears sit at. Re-pushed every
        // frame because the object is destroyed and rebuilt across every loading screen.
        void SetCameraObject(const void* camera);

        // Where the correction starts from: the camera, or wherever the client put it.
        //
        // The client's own answer is the CHARACTER. Sound_ListenerAtCharacter defaults on,
        // and its branch takes the POSITION from the player object and the ORIENTATION
        // from the camera - so the ears already turned with the camera before any of this,
        // but never moved with it. Flat and third-person that is a defensible choice; worn
        // it is not, because the viewpoint IS the head. Standing next to a fire and zooming
        // the camera a hundred yards out left the fire exactly as loud, and a real step in
        // the room is a yard against a falloff measured in tens of them.
        //
        // On, the base position becomes the camera's, which with the head displacement
        // added on top puts the listener where the eyes actually are.
        void SetEarsAtCamera(bool on);

        // Whether the head's displacement moves the listener at all. Off leaves only the
        // rotation, which is the half that needs no scale to be believed - useful for
        // telling a wrong distance from a wrong direction.
        void SetTrackPosition(bool on);

        // Which way round each axis of the head runs against the client's world. Exposed
        // rather than baked in for the reason every sign in this project is: a sign is
        // the one thing worth being able to flip in the headset without a rebuild.
        //
        // Yaw and pitch start at the values the culling rotation settled on, since both
        // turn the same world by the same head. The lateral sign is a separate
        // assumption - that the client's world right is 90 degrees clockwise of its
        // forward - and is the one to flip if walking left makes sounds move left.
        void SetYawSign(float sign);
        void SetPitchSign(float sign);
        void SetRollSign(float sign);
        void SetLateralSign(float sign);

        // A fixed extra turn on top of the head's, in degrees, for diagnosis.
        //
        // This is how the whole feature gets tested with the headset sitting still: park
        // the character next to something that loops - a forge, a waterfall, a candle -
        // and force 90 degrees. If the listener is following, the sound moves to one ear
        // and stays there. Nothing else here can hold a sound field still enough to
        // judge, and a turntable turns the head and the picture together.
        void SetForcedYawDegrees(float degrees);

        // Logs the next few listener updates as the client asked for them and as they
        // came out, which is the only way to tell "the hook is not firing" from "it
        // fires and the correction is zero" from "it fires with the head at the wrong
        // sign". All three sound identical in the headset.
        void RequestDump() { m_dumpCalls = 8; }

        void Report() const;

        // Called by the detour, on the client's own thread. Public only because a free
        // function cannot reach a private member.
        int OnSetListener(const float* position, const float* forward, const float* up,
                          const void* velocity);

    private:
        bool ApplyPatch(bool on);
        bool BuildTrampoline();

        bool m_patched = false;
        bool m_active = false;
        bool m_trackPosition = true;
        bool m_earsAtCamera = true;
        const uint8_t* m_camera = nullptr;

        float m_headYaw = 0.0f;
        float m_headPitch = 0.0f;
        float m_headRoll = 0.0f;
        Vec3 m_headOffset;              // metres, neutral frame
        float m_unitsPerMetre = 1.0936f;

        float m_yawSign = 1.0f;
        float m_pitchSign = 1.0f;
        float m_rollSign = 1.0f;
        float m_lateralSign = 1.0f;
        float m_forcedYaw = 0.0f;

        int m_dumpCalls = 0;
        uint64_t m_totalCalls = 0;
        uint64_t m_totalCorrected = 0;
        float m_lastYawApplied = 0.0f;
        float m_lastPitchApplied = 0.0f;
        Vec3 m_lastMoveYards;

        // How far the client's own base position sat from the camera. In third person
        // this is the zoom distance, and it is the number that says whether moving the
        // ears to the viewpoint changed anything at all.
        float m_lastBaseSeparation = 0.0f;
        float m_lastPosition[3] = { 0.0f, 0.0f, 0.0f };
        float m_lastForward[3] = { 0.0f, 0.0f, 0.0f };
    };

    SoundListener& HeadListener();
}
