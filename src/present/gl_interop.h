#pragma once

#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace wowvr
{
    // Zero-copy hand-off for a plain (non-Ex) D3D9 device, which cannot create
    // shared surfaces (the runtime rejects pSharedHandle with D3DERR_INVALIDCALL).
    // WGL_NV_DX_interop exists for exactly this situation: a GL texture is aliased
    // onto the D3D9 render-target texture, and the compositor is handed the GL
    // texture. The frame never leaves the GPU.
    //
    // Thread affinity: Init creates a GL context on the calling thread and leaves
    // it current there. Everything else - registration, locking, and the OpenVR
    // Submit that consumes GlTextureName - must happen on that same thread. The
    // Present hook thread is the only caller, so this holds by construction.
    class GlInterop
    {
    public:
        // Creates a hidden window and GL context, resolves the interop extension
        // and opens an interop handle onto the game's device. Failing any step
        // (non-NVIDIA-style driver without the extension, remote session, ...)
        // shuts down cleanly and returns false; the caller falls back to copying.
        bool Init(IDirect3DDevice9* gameDevice);
        void Shutdown();

        bool IsActive() const { return m_interopDevice != nullptr; }

        // Aliases a GL texture onto the eye's render-target texture. Registration
        // pins the D3D9 texture, so this must be undone before a device reset.
        bool RegisterEyeTexture(int eye, IDirect3DTexture9* texture);
        void UnregisterEyeTextures();

        bool HasEyeTextures() const { return m_interopObject[0] != nullptr; }

        // Grants GL access to the registered textures for the span of a lock. The
        // lock also synchronises: the driver completes all pending D3D9 rendering
        // into the textures first, which replaces any explicit event-query fence.
        bool LockEyes();
        void UnlockEyes();

        uint32_t GlTextureName(int eye) const
        {
            return (eye >= 0 && eye < 2) ? m_glTexture[eye] : 0;
        }

    private:
        bool EnsureContextCurrent();

        void* m_window = nullptr;        // HWND
        void* m_dc = nullptr;            // HDC
        void* m_glContext = nullptr;     // HGLRC
        void* m_interopDevice = nullptr; // wglDXOpenDeviceNV handle
        void* m_interopObject[2] = {};
        uint32_t m_glTexture[2] = {};
        bool m_lockFailureLogged = false;
    };
}
