#pragma once

#include "core/math3d.h"

namespace wowvr
{
    // Turns the client's spherical billboards (the sun, the moon, glows, M2 parts
    // flagged to always face the viewer) to face the HEAD instead of the game camera.
    //
    // The M2 bone animator (0x0082F0F0) handles bone flag 0x8 by overwriting the
    // bone's rotation, in VIEW space, with a fixed one - rows (0,0,-1), (1,0,0),
    // (0,1,0) - so the sprite lies flat in the game camera's view plane. In a headset
    // the world is turned on into the head's frame afterwards, so a sprite anywhere
    // but straight down the camera's axis is seen at an angle: foreshortened, and
    // swinging as the character turns. Both ways that case is written end in a jump to
    // the shared scale-and-finish code at 0x008301FC (from 0x00830071 and 0x0083009D);
    // those two jumps are redirected through a detour that turns the three rows by
    // the inverse of the head correction first, which puts the sprite flat in the
    // head's view plane instead. Cylindrical billboards (0x10/0x20/0x40) are left
    // alone - they keep an axis upright, which turning them would break.
    //
    // Only for scenes drawn into the world: the scene animator (0x00821A20) is entered
    // by the world, sky, camera and login-screen scenes, and also by interface model
    // frames (portraits, the dressing room), whose billboards are on the flat panel and
    // must keep facing it. The entry is detoured to note which caller it is.
    class BillboardFacing
    {
    public:
        bool Install();

        // Refreshed once a frame. 'gameViewToHead' is ProjectionPatch::GameViewToHead.
        void Update(bool active, const Mat4& gameViewToHead);

        void LogStatus() const;
    };

    BillboardFacing& Billboards();
}
