#pragma once

namespace wowvr
{
    // Keeps unit portraits on the client's render-to-texture path.
    //
    // The client draws a portrait one of two ways (0x00619580). Normally it renders the
    // model into a 64x64 render-target texture of its own. The fallback draws the model
    // into the BACK BUFFER and copies the pixels out of it. Under WoWVR the back buffer
    // is not where the client thinks: during the interface pass its draws go to the
    // panel's own surface, and the real back buffer holds the desktop mirror. So the
    // fallback copies whatever the mirror happened to hold into the portrait - interface
    // pieces, or nothing - and the model it drew lands on the panel for a frame, which
    // is the full-screen flash on a newly selected target.
    //
    // Which way is decided once, at 0x00616DC0, and stored at 0x00C5CDFC. That test
    // reads back a 64x64 patch of the screen and asks whether any pixel's alpha byte is
    // not 0xFF. The back buffer is X8R8G8B8, so that byte is undefined: the native
    // NVIDIA driver hands back something other than 0xFF and the client picks render to
    // texture; DXVK, correctly, hands back 0xFF and the client picks the fallback. That
    // is why portraits broke only under DXVK.
    //
    // The patch makes the test's verdict "render to texture", which is what it already
    // is natively. The test's other early-out (an API without the capability at all)
    // is left in place.
    class PortraitFix
    {
    public:
        // Applied before the device exists, so the client's one-time test sees it.
        // Verifies the bytes first; a different client is left alone.
        bool Install();
    };

    PortraitFix& Portraits();
}
