#pragma once

#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace wowvr
{
    // The Ctrl+Alt+F1 help card: every WoWVR hotkey with a few words on what it does,
    // as one texture drawn into the interface panel (so it shows in the headset, in
    // the overlay and on the desktop mirror alike).
    //
    // The text is rasterised once with GDI - antialiased white on black, the grey level
    // taken as coverage - over a translucent dark backing, in straight (not
    // premultiplied) alpha, the same convention the pointer art uses.
    class HelpCard
    {
    public:
        // Builds the texture on first use, or again if the device has changed. The
        // texture lives in the managed pool, so device resets do not touch it.
        IDirect3DTexture9* Texture(IDirect3DDevice9* device);

        uint32_t Width() const { return m_width; }
        uint32_t Height() const { return m_height; }

        bool Visible() const { return m_visible; }
        void Toggle() { m_visible = !m_visible; }

    private:
        IDirect3DDevice9* m_device = nullptr;
        IDirect3DTexture9* m_texture = nullptr;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        bool m_visible = false;
        bool m_failed = false;
    };

    HelpCard& Help();
}
