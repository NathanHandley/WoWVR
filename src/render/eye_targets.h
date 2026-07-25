#pragma once

#include <windows.h>

#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DSurface9;

namespace wowvr
{
    // The D3D9 half of the frame hand-off: rescales the finished back buffer into an
    // eye-sized render target and reads it back into system memory so the presenter
    // can push it across to D3D11.
    class EyeTargets
    {
    public:
        bool Create(IDirect3DDevice9* device, uint32_t width, uint32_t height);
        void Destroy();

        bool IsReady() const { return m_scaled != nullptr && m_staging != nullptr; }
        uint32_t Width() const { return m_width; }
        uint32_t Height() const { return m_height; }

        // Resolves and rescales back buffer 0 into our staging surface.
        bool CaptureBackBuffer(IDirect3DDevice9* device);

        // Same, but from a region of an arbitrary render target. Used to pull one eye
        // out of the side-by-side stereo target.
        bool CaptureRegion(IDirect3DDevice9* device, IDirect3DSurface9* source,
                           const RECT* sourceRect);

        bool Lock();
        void Unlock();
        const void* LockedPixels() const { return m_lockedPixels; }
        uint32_t LockedPitch() const { return m_lockedPitch; }

    private:
        IDirect3DSurface9* m_scaled = nullptr;    // D3DPOOL_DEFAULT render target
        IDirect3DSurface9* m_staging = nullptr;   // D3DPOOL_SYSTEMMEM readback target

        uint32_t m_width = 0;
        uint32_t m_height = 0;

        void* m_lockedPixels = nullptr;
        uint32_t m_lockedPitch = 0;

        bool m_captureFailureLogged = false;
    };
}
