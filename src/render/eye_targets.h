#pragma once

#include <windows.h>

#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DSurface9;
struct IDirect3DTexture9;

namespace wowvr
{
    // The D3D9 half of the frame hand-off: rescales the finished back buffer into an
    // eye-sized render target. In shared mode that render target is created with a
    // share handle and the presenter's D3D11 side opens the very same surface, so the
    // rescale is the last time the frame is touched. In copy mode it is read back
    // into system memory for the presenter to push across.
    class EyeTargets
    {
    public:
        enum Kind
        {
            KindCopy = 0,      // plain render target + system-memory readback
            KindSharedHandle,  // render-target texture created with a share handle
            KindTexture        // render-target texture, no handle (GL interop aliases it)
        };

        bool Create(IDirect3DDevice9* device, uint32_t width, uint32_t height, Kind kind);
        void Destroy();

        // Non-null exactly when Create succeeded in KindSharedHandle. The presenter
        // opens this with OpenSharedResource; it is not an NT handle, never closed.
        void* SharedHandle() const { return m_sharedHandle; }

        // The render-target texture, in the two kinds that have one.
        IDirect3DTexture9* RenderTargetTexture() const { return m_renderTexture; }

        // Copies the eye render target into the system-memory surface so Lock can
        // reach the pixels. The copy path does this inside Capture*; shared mode
        // needs it only for the occasional BMP dump.
        bool ReadBack(IDirect3DDevice9* device);

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
        IDirect3DTexture9* m_renderTexture = nullptr; // owns m_scaled in the texture kinds
        void* m_sharedHandle = nullptr;
        Kind m_kind = KindCopy;

        uint32_t m_width = 0;
        uint32_t m_height = 0;

        void* m_lockedPixels = nullptr;
        uint32_t m_lockedPitch = 0;

        bool m_captureFailureLogged = false;
    };
}
