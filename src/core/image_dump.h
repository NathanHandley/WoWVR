#pragma once

#include <cstdint>

namespace wowvr
{
    // Writes 32-bit BGRA pixels out as a BMP.
    //
    // This exists so that the contents of an eye buffer can be inspected without
    // wearing the headset. It is the only way to tell a working frame from a black
    // one, or an upside-down one, while iterating on the render path.
    bool SaveBgraBmp(const wchar_t* path, const void* pixels,
                     uint32_t width, uint32_t height, uint32_t rowPitch);
}
