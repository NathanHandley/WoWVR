#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace wowvr
{
    // Bridges D3D9 frames into the D3D11 textures that OpenVR's compositor accepts.
    //
    // OpenVR has no D3D9 texture type at all, so something has to cross the API
    // boundary. Two ways across:
    //
    // - Zero-copy: AdoptSharedTextures opens the share handles of the D3D9 eye
    //   render targets, so the compositor reads the same GPU memory the game's
    //   StretchRect wrote. Nothing touches system memory.
    // - Copy: CreateEyeTextures makes textures of our own and Upload fills them
    //   with CPU pixels read back from D3D9. The fallback when sharing fails.
    class Presenter
    {
    public:
        bool Init(int preferredAdapterIndex, uint32_t width, uint32_t height);
        void Shutdown();

        // Copy mode: textures this presenter owns and Upload fills.
        bool CreateEyeTextures();

        // Zero-copy mode: opens the D3D9 share handles as this device's textures.
        // Replaces whatever eye textures existed. Returns false with everything
        // released if the driver refuses, so the caller can fall back to copy mode.
        bool AdoptSharedTextures(void* leftHandle, void* rightHandle);

        // Adopted textures alias D3D9 surfaces that die on a device reset, so they
        // are dropped with the other frame resources. Own (copy-mode) textures
        // survive resets and are kept.
        void ReleaseAdoptedTextures();

        bool HasEyeTextures() const { return m_eyeTexture[0] != nullptr; }

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
        ID3D11Texture2D* m_stagingTexture[2] = {};  // copy mode only
        bool m_adopted = false;

        uint32_t m_width = 0;
        uint32_t m_height = 0;
    };
}
