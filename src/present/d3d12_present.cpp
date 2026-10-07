#include "present/d3d12_present.h"

#include "core/log.h"
#include "vr/vr_session.h"

#include <windows.h>

#include <d3d9.h>
#include <d3d9on12.h>
#include <d3d12.h>

#include <openvr.h>

namespace wowvr
{
    bool D3D12Present::Init(IDirect3DDevice9* gameDevice)
    {
        if (IsActive())
        {
            return true;
        }
        if (gameDevice == nullptr)
        {
            return false;
        }

        // Only succeeds when the d3d9 runtime was created through
        // Direct3DCreate9On12; a native-driver device does not know this interface.
        HRESULT hr = gameDevice->QueryInterface(__uuidof(IDirect3DDevice9On12),
                                                reinterpret_cast<void**>(&m_device9On12));
        if (FAILED(hr) || m_device9On12 == nullptr)
        {
            return false;
        }

        hr = m_device9On12->GetD3D12Device(__uuidof(ID3D12Device),
                                           reinterpret_cast<void**>(&m_device12));
        if (FAILED(hr) || m_device12 == nullptr)
        {
            WOWVR_WARN("D3D9On12 present: GetD3D12Device failed (0x%08lx).", hr);
            Shutdown();
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = m_device12->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue),
                                            reinterpret_cast<void**>(&m_queue));
        if (FAILED(hr) || m_queue == nullptr)
        {
            WOWVR_WARN("D3D9On12 present: command queue creation failed (0x%08lx).", hr);
            Shutdown();
            return false;
        }

        hr = m_device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                     reinterpret_cast<void**>(&m_fence));
        if (FAILED(hr) || m_fence == nullptr)
        {
            WOWVR_WARN("D3D9On12 present: fence creation failed (0x%08lx).", hr);
            Shutdown();
            return false;
        }

        m_fenceValue = 0;
        WOWVR_INFO("D3D9On12 present ready; frames hand over as D3D12 resources (zero-copy).");
        return true;
    }

    void D3D12Present::Shutdown()
    {
        // Let the compositor's queued copies finish before the queue and fence go.
        if (m_fence != nullptr && m_fence->GetCompletedValue() < m_fenceValue)
        {
            HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (done != nullptr)
            {
                if (SUCCEEDED(m_fence->SetEventOnCompletion(m_fenceValue, done)))
                {
                    WaitForSingleObject(done, 1000);
                }
                CloseHandle(done);
            }
        }
        if (m_fence != nullptr)
        {
            m_fence->Release();
            m_fence = nullptr;
        }
        if (m_queue != nullptr)
        {
            m_queue->Release();
            m_queue = nullptr;
        }
        if (m_device12 != nullptr)
        {
            m_device12->Release();
            m_device12 = nullptr;
        }
        if (m_device9On12 != nullptr)
        {
            m_device9On12->Release();
            m_device9On12 = nullptr;
        }
    }

    bool D3D12Present::SubmitEyes(IDirect3DTexture9* left, IDirect3DTexture9* right, bool stereo)
    {
        if (!IsActive() || left == nullptr || (stereo && right == nullptr))
        {
            return false;
        }

        IDirect3DTexture9* textures[2] = { left, stereo ? right : left };
        const int uniqueCount = stereo ? 2 : 1;

        // Unwrap makes m_queue wait for all pending 9on12 rendering into the
        // resource - the StretchRects that filled it - so no explicit fence is
        // needed on the way in. The resource arrives in the COMMON state, from
        // which the compositor's read promotes implicitly.
        ID3D12Resource* resources[2] = {};
        for (int i = 0; i < uniqueCount; ++i)
        {
            const HRESULT hr = m_device9On12->UnwrapUnderlyingResource(
                textures[i], m_queue, __uuidof(ID3D12Resource),
                reinterpret_cast<void**>(&resources[i]));
            if (FAILED(hr) || resources[i] == nullptr)
            {
                if (!m_submitFailureLogged)
                {
                    WOWVR_ERROR("D3D9On12 present: UnwrapUnderlyingResource failed (0x%08lx); "
                                "no frames will reach the headset.", hr);
                    m_submitFailureLogged = true;
                }
                if (i > 0)
                {
                    UINT64 none = 0;
                    m_device9On12->ReturnUnderlyingResource(textures[0], 0, &none, nullptr);
                    resources[0]->Release();
                }
                return false;
            }
        }

        bool submitted = true;
        for (int eye = 0; eye < 2; ++eye)
        {
            vr::D3D12TextureData_t data = {};
            data.m_pResource = resources[stereo ? eye : 0];
            data.m_pCommandQueue = m_queue;
            data.m_nNodeMask = 0;
            submitted = Vr().SubmitEyeD3D12(eye, &data) && submitted;
        }

        // The compositor enqueued its reads onto m_queue inside Submit, so a
        // signal here fences everything it will do with the resource. Handing that
        // fence to ReturnUnderlyingResource makes 9on12 wait for the compositor
        // before it touches the texture again.
        ++m_fenceValue;
        m_queue->Signal(m_fence, m_fenceValue);

        for (int i = 0; i < uniqueCount; ++i)
        {
            UINT64 signalValue = m_fenceValue;
            ID3D12Fence* fence = m_fence;
            m_device9On12->ReturnUnderlyingResource(textures[i], 1, &signalValue, &fence);
            resources[i]->Release();
        }

        return submitted;
    }
}
