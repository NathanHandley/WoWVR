#pragma once

#include "core/math3d.h"
#include "ui/ui_panel.h"

namespace wowvr
{
    // Makes the client point into the world, and place things over the world, along
    // the line the player actually looks down: from the HEAD, through the interface
    // panel, out into the scene.
    //
    // The client does both with its own flat camera. The mouse becomes a ray through
    // that camera's frustum, and nameplates, floating combat text and chat bubbles are
    // placed where that frustum projects the creature. In a headset that frustum is
    // never shown - the world is drawn per eye, turned by the head - and WoWVR widens
    // it for culling besides, so a pointer resting on a creature in the headset picks
    // something else entirely and its nameplate floats somewhere unrelated.
    //
    // Both directions go through one pair of client functions, so two detours fix
    // every consumer at once:
    //
    //   0x004BF0F0  screen -> ray. Called only by CGWorldFrame's 0x004F6450, from the
    //               mouse-over and click pick (0x004F9DA0). cdecl (nx, ny, start*, end*)
    //               with nx, ny in 0..1 across the world frame, y up; writes the ray's
    //               ends relative to the camera, which the caller then adds.
    //   0x004F6D20  world -> screen, CGWorldFrame::GetScreenCoordinates. thiscall
    //               (world*, out*, flags*), ret 0xC, returns bool in al. Used by
    //               nameplates (0x0072B350 -> 0x00715720), WorldText, chat bubbles,
    //               GameUI and - which keeps picking self-consistent - the pick trace
    //               itself (0x004F9930).
    //
    // Replacing them with "intersect the head's ray with the panel" makes them exact
    // inverses of each other again, just as the client's own pair are on a monitor.
    //
    // Installed once VR is running; when inactive (no panel, VR off, WorldPointing=0)
    // both detours hand the call straight to the client's own code.
    // A lifebar (nameplate) the client placed this frame, for drawing in the world.
    struct WorldPlate
    {
        Vec3 body;              // the point it hangs from, metres, body frame
        float distanceMetres;   // from the head
        float slotU;            // where in the interface image its anchor was put: the
        float slotV;            //   cell's centre across, anchorV of the way down (0..1)
    };

    class WorldPointer
    {
    public:
        // Patches both entry points if they still hold the bytes this was written
        // against. Idempotent; false (and logged) if either is not what was expected.
        bool Install();

        // The state the detours work from, refreshed once a frame on the render thread
        // - which in this client is also the thread that runs the UI and the picks.
        //
        // 'headBody' is the head's position in body metres, 'gameViewToBody' the
        // rotation from the game camera's view space into the body frame.
        void Update(bool active, const PanelShape& shape, const Vec3& headBody,
                    const Mat4& gameViewToBody, float unitsPerMetre);

        // Hands both entry points straight back to the client's own code.
        void Deactivate();

        void LogStatus() const;

        // Lifebars in the world. While on, every lifebar the client places is sent to its
        // own cell of a strip of the interface image (rows of 'columns' cells, each
        // cellU x cellV of the image, starting at stripTopV) instead of to where its unit
        // is, and the unit's position is remembered here. The caller cuts the strip out
        // of the interface, clears it, and draws each cell over its unit in 3D.
        // anchorV: how far down its cell a lifebar's anchor goes (the client hangs the
        // lifebar below its anchor point).
        void SetPlateStrip(bool on, float stripTopV, float cellU, float cellV, int columns,
                           int rows, float anchorV);
        bool PlateStripOn() const;
        int PlateCount() const;
        const WorldPlate& Plate(int index) const;
        // Once a frame, after the frame's plates have been drawn.
        void BeginPlateFrame();
    };

    WorldPointer& Pointer();

    // The geometry the detours run on, kept free of client memory so it can be
    // exercised on its own.
    namespace pointing
    {
        // The game camera, in world coordinates: WoW's right-handed, Z-up frame.
        struct CameraBasis
        {
            Vec3 position;
            Vec3 forward;
            Vec3 right;
            Vec3 up;
            float nearPlane = 0.0f;
            float farPlane = 0.0f;
        };

        struct Frame
        {
            PanelShape shape;
            Vec3 headBody;                          // metres, body frame
            Mat4 gameViewToBody = Mat4Identity();
            Mat4 bodyToGameView = Mat4Identity();
            float unitsPerMetre = 1.0936f;
        };

        // The ray from the head through panel point (u, v), as an origin relative to
        // the camera position and a unit direction, both in world coordinates.
        bool PanelToWorldRay(const Frame& frame, const CameraBasis& camera, float u, float v,
                             Vec3& originRelative, Vec3& direction);

        // Where the head sees a world point on the panel. 'distanceYards' is the
        // point's distance from the head. False if the line never meets the panel's
        // cylinder in front of the head.
        bool WorldToPanel(const Frame& frame, const CameraBasis& camera, const Vec3& world,
                          float& u, float& v, float& distanceYards);

        // A world point in the body frame, metres.
        void WorldToBody(const Frame& frame, const CameraBasis& camera, const Vec3& world,
                         Vec3& body);
    }
}
