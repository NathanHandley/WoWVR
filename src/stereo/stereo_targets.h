#pragma once

#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DSurface9;

namespace wowvr
{
    // A single double-wide render target holding both eyes side by side.
    //
    // The alternative was a separate target per eye, but the game issues around 560
    // world draws a frame and each one has to be drawn twice, which would mean over a
    // thousand render-target switches per frame. In D3D9 those flush the pipeline and
    // are ruinously expensive. Sharing one surface means the per-draw cost is a
    // viewport change and a constant upload instead, and OpenVR can be handed each
    // half directly through VRTextureBounds_t.
    class StereoTargets
    {
    public:
        bool Create(IDirect3DDevice9* device, uint32_t eyeWidth, uint32_t eyeHeight);
        void Destroy();

        bool IsReady() const { return m_color != nullptr && m_depth != nullptr; }

        uint32_t EyeWidth() const { return m_eyeWidth; }
        uint32_t EyeHeight() const { return m_eyeHeight; }

        IDirect3DSurface9* Color() const { return m_color; }
        IDirect3DSurface9* Depth() const { return m_depth; }

    private:
        IDirect3DSurface9* m_color = nullptr;
        IDirect3DSurface9* m_depth = nullptr;
        uint32_t m_eyeWidth = 0;
        uint32_t m_eyeHeight = 0;
    };
}
