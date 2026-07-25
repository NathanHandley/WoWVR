#include "present/presenter.h"

#include "core/log.h"

#include <windows.h>

#include <iterator>

#include <d3d11.h>
#include <dxgi.h>

namespace wowvr
{
    namespace
    {
        // Picks the adapter the headset is plugged into. Falls back to the default
        // adapter, which is the right answer on a single-GPU machine anyway.
        IDXGIAdapter* AcquireAdapter(int preferredAdapterIndex)
        {
            if (preferredAdapterIndex < 0)
            {
                return nullptr;
            }

            IDXGIFactory1* factory = nullptr;
            if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
            {
                return nullptr;
            }

            IDXGIAdapter* adapter = nullptr;
            if (FAILED(factory->EnumAdapters(static_cast<UINT>(preferredAdapterIndex), &adapter)))
            {
                adapter = nullptr;
            }

            factory->Release();
            return adapter;
        }
    }

    bool Presenter::Init(int preferredAdapterIndex, uint32_t width, uint32_t height)
    {
        Shutdown();

        if (width == 0 || height == 0)
        {
            WOWVR_ERROR("Presenter asked for a zero-sized eye target (%ux%u).", width, height);
            return false;
        }

        m_width = width;
        m_height = height;

        IDXGIAdapter* adapter = AcquireAdapter(preferredAdapterIndex);
        const D3D_DRIVER_TYPE driverType =
            (adapter != nullptr) ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;

        const D3D_FEATURE_LEVEL requestedLevels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };

        D3D_FEATURE_LEVEL achievedLevel = D3D_FEATURE_LEVEL_10_0;
        const HRESULT hr = D3D11CreateDevice(
            adapter, driverType, nullptr, 0,
            requestedLevels, static_cast<UINT>(std::size(requestedLevels)),
            D3D11_SDK_VERSION, &m_device, &achievedLevel, &m_context);

        if (adapter != nullptr)
        {
            adapter->Release();
        }

        if (FAILED(hr) || m_device == nullptr)
        {
            WOWVR_ERROR("D3D11CreateDevice failed (0x%08lx). VR output is unavailable.", hr);
            Shutdown();
            return false;
        }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        // Matches D3DFMT_A8R8G8B8 byte for byte, so the readback is a straight copy.
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        for (int eye = 0; eye < 2; ++eye)
        {
            const HRESULT textureResult = m_device->CreateTexture2D(&desc, nullptr, &m_eyeTexture[eye]);
            if (FAILED(textureResult))
            {
                WOWVR_ERROR("Could not create the %s eye texture (0x%08lx).",
                            eye == 0 ? "left" : "right", textureResult);
                Shutdown();
                return false;
            }
        }

        WOWVR_INFO("Presenter ready: D3D11 feature level 0x%04x, %ux%u per eye.",
                   static_cast<unsigned>(achievedLevel), m_width, m_height);
        return true;
    }

    void Presenter::ReleaseTextures()
    {
        for (int eye = 0; eye < 2; ++eye)
        {
            if (m_eyeTexture[eye] != nullptr)
            {
                m_eyeTexture[eye]->Release();
                m_eyeTexture[eye] = nullptr;
            }
        }
    }

    void Presenter::Shutdown()
    {
        ReleaseTextures();

        if (m_context != nullptr)
        {
            m_context->Release();
            m_context = nullptr;
        }
        if (m_device != nullptr)
        {
            m_device->Release();
            m_device = nullptr;
        }

        m_width = 0;
        m_height = 0;
    }

    bool Presenter::Upload(int eye, const void* pixels, uint32_t sourceRowPitch)
    {
        if (m_context == nullptr || eye < 0 || eye > 1 || m_eyeTexture[eye] == nullptr || pixels == nullptr)
        {
            return false;
        }

        m_context->UpdateSubresource(m_eyeTexture[eye], 0, nullptr, pixels, sourceRowPitch, 0);
        return true;
    }

    void* Presenter::EyeTexture(int eye) const
    {
        if (eye < 0 || eye > 1)
        {
            return nullptr;
        }
        return m_eyeTexture[eye];
    }
}
