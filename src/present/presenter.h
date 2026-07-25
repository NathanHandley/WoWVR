#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace wowvr
{
    // Bridges D3D9 pixels into the D3D11 textures that OpenVR's compositor accepts.
    //
    // OpenVR has no D3D9 texture type at all, so something has to cross the API
    // boundary. This first implementation takes the safe route: the caller hands us
    // CPU-side pixels read back from a D3D9 surface and we push them into a D3D11
    // texture. It costs a round trip through system memory every frame, which is the
    // price of being certain it works everywhere. Phase 6 replaces the innards with a
    // shared-surface path (D3D9On12) behind this same interface.
    class Presenter
    {
    public:
        bool Init(int preferredAdapterIndex, uint32_t width, uint32_t height);
        void Shutdown();

        bool IsReady() const { return m_device != nullptr; }

        uint32_t Width() const { return m_width; }
        uint32_t Height() const { return m_height; }

        // 'pixels' must be tightly packed BGRA8 rows of at least Width()*4 bytes.
        bool Upload(int eye, const void* pixels, uint32_t sourceRowPitch);

        // The ID3D11Texture2D to hand to IVRCompositor::Submit.
        void* EyeTexture(int eye) const;

    private:
        void ReleaseTextures();

        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;
        ID3D11Texture2D* m_eyeTexture[2] = {};

        uint32_t m_width = 0;
        uint32_t m_height = 0;
    };
}
