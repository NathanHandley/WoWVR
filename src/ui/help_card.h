#pragma once

#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace wowvr
{
    // A card of text drawn into the interface panel, so it shows in the headset, in the
    // overlay and on the desktop mirror alike.
    //
    // The text is rasterised once with GDI - antialiased, the grey level taken as
    // coverage - over a translucent dark backing, in straight (not premultiplied)
    // alpha, the same convention the pointer art uses. The texture lives in the
    // managed pool, so device resets do not touch it.
    class TextCard
    {
    public:
        enum Kind
        {
            CommandList,   // Ctrl+Alt+F1: every hotkey with a few words on each
            LaunchHint     // one line saying Ctrl+Alt+F1 opens that list
        };

        explicit TextCard(Kind kind) : m_kind(kind) {}

        // Builds the texture on first use, or again if the device has changed.
        IDirect3DTexture9* Texture(IDirect3DDevice9* device);

        uint32_t Width() const { return m_width; }
        uint32_t Height() const { return m_height; }

    private:
        Kind m_kind;
        IDirect3DDevice9* m_device = nullptr;
        IDirect3DTexture9* m_texture = nullptr;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        bool m_failed = false;
    };

    // The Ctrl+Alt+F1 command list, and whether it is showing.
    class HelpCard : public TextCard
    {
    public:
        HelpCard() : TextCard(CommandList) {}
        bool Visible() const { return m_visible; }
        void Toggle() { m_visible = !m_visible; }

    private:
        bool m_visible = false;
    };

    HelpCard& Help();
    TextCard& LaunchHintCard();
}
