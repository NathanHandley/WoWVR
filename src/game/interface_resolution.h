#pragma once

#include <cstdint>

namespace wowvr
{
    // Lets the client run at a resolution the monitor cannot show (Cfg().interfaceWidth x
    // Cfg().interfaceHeight), so the interface panel in the headset gets more pixels while
    // the desktop window still fits the screen.
    //
    // The client decides its resolution in four places, and each is answered here through
    // its own imports, so nothing outside Wow.exe sees a changed answer:
    //   - EnumDisplaySettingsA: the resolution list gxResolution is checked against gains
    //     the configured size, or the client falls back to a mode it does know.
    //   - GetMonitorInfoA: the work area the client fits its window into (0x00684D70) is
    //     reported big enough that the size is not shrunk to the screen.
    //   - CreateWindowExA: the game window itself is then created shrunk to fit the real
    //     work area, aspect kept. The client keeps the full size as its own resolution, so
    //     the back buffer is that size and Present scales it into the smaller window.
    //   - GetClientRect: on the game window, the full size - the client sizes its back
    //     buffer from it. Mouse positions are left alone; the client normalises them
    //     against the real window itself (measured).
    //
    // Installed from DllMain, before the client builds its mode list or its window.
    void InstallInterfaceResolution();

    // Game pixels per pixel of the real window across: 3200 / 2490 while the game runs
    // bigger than its window, 1 otherwise. The panel in the headset divides its width by
    // this, so the extra pixels make the interface sharper rather than bigger.
    float InterfacePixelScale();
}
