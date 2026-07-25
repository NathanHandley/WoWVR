#include "stereo/stereo_targets.h"

#include "core/log.h"

#include <windows.h>

#include <d3d9.h>

namespace wowvr
{
    bool StereoTargets::Create(IDirect3DDevice9* device, uint32_t eyeWidth, uint32_t eyeHeight)
    {
        Destroy();

        if (device == nullptr || eyeWidth == 0 || eyeHeight == 0)
        {
            return false;
        }

        m_eyeWidth = eyeWidth;
        m_eyeHeight = eyeHeight;

        const uint32_t totalWidth = eyeWidth * 2;

        HRESULT hr = device->CreateRenderTarget(totalWidth, eyeHeight, D3DFMT_A8R8G8B8,
                                                D3DMULTISAMPLE_NONE, 0, FALSE, &m_color, nullptr);
        if (FAILED(hr))
        {
            WOWVR_ERROR("Could not create the %ux%u stereo render target (0x%08lx).",
                        totalWidth, eyeHeight, hr);
            Destroy();
            return false;
        }

        // Matches the client's own AutoDepthStencilFormat (D3DFMT_D24X8) so that depth
        // behaves exactly as the game expects.
        hr = device->CreateDepthStencilSurface(totalWidth, eyeHeight, D3DFMT_D24X8,
                                               D3DMULTISAMPLE_NONE, 0, TRUE, &m_depth, nullptr);
        if (FAILED(hr))
        {
            hr = device->CreateDepthStencilSurface(totalWidth, eyeHeight, D3DFMT_D24S8,
                                                   D3DMULTISAMPLE_NONE, 0, TRUE, &m_depth, nullptr);
        }
        if (FAILED(hr))
        {
            WOWVR_ERROR("Could not create the stereo depth buffer (0x%08lx).", hr);
            Destroy();
            return false;
        }

        WOWVR_INFO("Stereo targets created: %ux%u total, %ux%u per eye.",
                   totalWidth, eyeHeight, eyeWidth, eyeHeight);
        return true;
    }

    void StereoTargets::Destroy()
    {
        if (m_color != nullptr)
        {
            m_color->Release();
            m_color = nullptr;
        }
        if (m_depth != nullptr)
        {
            m_depth->Release();
            m_depth = nullptr;
        }
        m_eyeWidth = 0;
        m_eyeHeight = 0;
    }
}
