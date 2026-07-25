#include "render/eye_targets.h"

#include "core/log.h"

#include <windows.h>

#include <d3d9.h>

namespace wowvr
{
    bool EyeTargets::Create(IDirect3DDevice9* device, uint32_t width, uint32_t height)
    {
        Destroy();

        if (device == nullptr || width == 0 || height == 0)
        {
            return false;
        }

        m_width = width;
        m_height = height;

        // Non-multisampled on purpose: StretchRect from the back buffer into this
        // surface resolves any multisampling the game asked for, which is what makes
        // the GetRenderTargetData readback below legal.
        HRESULT hr = device->CreateRenderTarget(
            width, height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0,
            FALSE, &m_scaled, nullptr);
        if (FAILED(hr))
        {
            WOWVR_ERROR("CreateRenderTarget(%ux%u) failed (0x%08lx).", width, height, hr);
            Destroy();
            return false;
        }

        hr = device->CreateOffscreenPlainSurface(
            width, height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &m_staging, nullptr);
        if (FAILED(hr))
        {
            WOWVR_ERROR("CreateOffscreenPlainSurface(%ux%u) failed (0x%08lx).", width, height, hr);
            Destroy();
            return false;
        }

        WOWVR_INFO("Eye targets created at %ux%u.", width, height);
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
