#include "render/eye_targets.h"

#include "core/log.h"

#include <windows.h>

#include <d3d9.h>

namespace wowvr
{
    bool EyeTargets::Create(IDirect3DDevice9* device, uint32_t width, uint32_t height, Kind kind)
    {
        Destroy();

        if (device == nullptr || width == 0 || height == 0)
        {
            return false;
        }

        m_width = width;
        m_height = height;
        m_kind = kind;

        HRESULT hr;
        if (kind != KindCopy)
        {
            // A render-target texture the frame is left in for someone else to read
            // in place - the D3D11 presenter through a share handle, or a GL alias
            // through WGL_NV_DX_interop. Either way the StretchRect into it is the
            // last time the frame is touched: no readback, no upload.
            //
            // The share handle only exists on a D3D9Ex device; plain D3D9 rejects
            // it with D3DERR_INVALIDCALL, which is why KindTexture exists.
            HANDLE handle = nullptr;
            hr = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                                       D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                       &m_renderTexture,
                                       (kind == KindSharedHandle) ? &handle : nullptr);
            if (FAILED(hr) || m_renderTexture == nullptr
                || (kind == KindSharedHandle && handle == nullptr))
            {
                WOWVR_WARN("Eye render-target texture (%ux%u%s) unavailable (0x%08lx).",
                           width, height,
                           (kind == KindSharedHandle) ? ", shared" : "", hr);
                Destroy();
                return false;
            }

            hr = m_renderTexture->GetSurfaceLevel(0, &m_scaled);
            if (FAILED(hr) || m_scaled == nullptr)
            {
                WOWVR_ERROR("GetSurfaceLevel on the eye texture failed (0x%08lx).", hr);
                Destroy();
                return false;
            }

            m_sharedHandle = handle;
        }
        else
        {
            // Non-multisampled on purpose: StretchRect from the back buffer into this
            // surface resolves any multisampling the game asked for, which is what makes
            // the GetRenderTargetData readback below legal.
            hr = device->CreateRenderTarget(
                width, height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0,
                FALSE, &m_scaled, nullptr);
            if (FAILED(hr))
            {
                WOWVR_ERROR("CreateRenderTarget(%ux%u) failed (0x%08lx).", width, height, hr);
                Destroy();
                return false;
            }
        }

        hr = device->CreateOffscreenPlainSurface(
            width, height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &m_staging, nullptr);
        if (FAILED(hr))
        {
            WOWVR_ERROR("CreateOffscreenPlainSurface(%ux%u) failed (0x%08lx).", width, height, hr);
            Destroy();
            return false;
        }

        WOWVR_INFO("Eye targets created at %ux%u%s.", width, height,
                   (kind == KindSharedHandle) ? " (shared handle)"
                   : (kind == KindTexture)    ? " (texture)"
                                              : "");
        return true;
    }

    void EyeTargets::Destroy()
    {
        Unlock();

        if (m_scaled != nullptr)
        {
            m_scaled->Release();
            m_scaled = nullptr;
        }
        if (m_renderTexture != nullptr)
        {
            m_renderTexture->Release();
            m_renderTexture = nullptr;
        }
        // The share handle is a kernel identifier, not an NT handle; nothing to close.
        m_sharedHandle = nullptr;
        m_kind = KindCopy;
        if (m_staging != nullptr)
        {
            m_staging->Release();
            m_staging = nullptr;
        }

        m_width = 0;
        m_height = 0;
        m_captureFailureLogged = false;
    }

    bool EyeTargets::CaptureBackBuffer(IDirect3DDevice9* device)
    {
        if (device == nullptr || !IsReady())
        {
            return false;
        }

        IDirect3DSurface9* backBuffer = nullptr;
        HRESULT hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (FAILED(hr) || backBuffer == nullptr)
        {
            if (!m_captureFailureLogged)
            {
                WOWVR_ERROR("GetBackBuffer failed (0x%08lx); no frames will reach the headset.", hr);
                m_captureFailureLogged = true;
            }
            return false;
        }

        hr = device->StretchRect(backBuffer, nullptr, m_scaled, nullptr, D3DTEXF_LINEAR);
        if (FAILED(hr))
        {
            // Some drivers refuse a filtered stretch between certain formats; a point
            // sample is always available and is better than dropping the frame.
            hr = device->StretchRect(backBuffer, nullptr, m_scaled, nullptr, D3DTEXF_NONE);
        }
        backBuffer->Release();

        if (FAILED(hr))
        {
            if (!m_captureFailureLogged)
            {
                WOWVR_ERROR("StretchRect into the eye target failed (0x%08lx).", hr);
                m_captureFailureLogged = true;
            }
            return false;
        }

        if (m_kind != KindCopy)
        {
            // The consumer reads this very surface, so the frame is already where
            // it needs to be.
            return true;
        }

        hr = device->GetRenderTargetData(m_scaled, m_staging);
        if (FAILED(hr))
        {
            if (!m_captureFailureLogged)
            {
                WOWVR_ERROR("GetRenderTargetData failed (0x%08lx).", hr);
                m_captureFailureLogged = true;
            }
            return false;
        }

        return true;
    }

    bool EyeTargets::CaptureRegion(IDirect3DDevice9* device, IDirect3DSurface9* source,
                                   const RECT* sourceRect)
    {
        if (device == nullptr || source == nullptr || !IsReady())
        {
            return false;
        }

        HRESULT hr = device->StretchRect(source, sourceRect, m_scaled, nullptr, D3DTEXF_LINEAR);
        if (FAILED(hr))
        {
            hr = device->StretchRect(source, sourceRect, m_scaled, nullptr, D3DTEXF_NONE);
        }
        if (FAILED(hr))
        {
            if (!m_captureFailureLogged)
            {
                WOWVR_ERROR("StretchRect from the stereo target failed (0x%08lx).", hr);
                m_captureFailureLogged = true;
            }
            return false;
        }

        if (m_kind != KindCopy)
        {
            return true;
        }

        hr = device->GetRenderTargetData(m_scaled, m_staging);
        if (FAILED(hr))
        {
            if (!m_captureFailureLogged)
            {
                WOWVR_ERROR("GetRenderTargetData from the stereo target failed (0x%08lx).", hr);
                m_captureFailureLogged = true;
            }
            return false;
        }

        return true;
    }

    bool EyeTargets::ReadBack(IDirect3DDevice9* device)
    {
        if (device == nullptr || !IsReady())
        {
            return false;
        }
        return SUCCEEDED(device->GetRenderTargetData(m_scaled, m_staging));
    }

    bool EyeTargets::Lock()
    {
        if (m_staging == nullptr || m_lockedPixels != nullptr)
        {
            return m_lockedPixels != nullptr;
        }

        D3DLOCKED_RECT locked = {};
        const HRESULT hr = m_staging->LockRect(&locked, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr))
        {
            return false;
        }

        m_lockedPixels = locked.pBits;
        m_lockedPitch = static_cast<uint32_t>(locked.Pitch);
        return true;
    }

    void EyeTargets::Unlock()
    {
        if (m_staging != nullptr && m_lockedPixels != nullptr)
        {
            m_staging->UnlockRect();
        }
        m_lockedPixels = nullptr;
        m_lockedPitch = 0;
    }
}
