#pragma once

#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct IDirect3DDevice9On12;
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Fence;

namespace wowvr
{
    // The zero-copy hand-off when the client runs on the D3D9On12 mapping layer.
    //
    // A plain D3D9 device cannot share its allocations with anything (share
    // handles, and therefore also the GL interop, both fail on this client). But
    // when the d3d9 runtime is created through Direct3DCreate9On12, every D3D9
    // resource is really a D3D12 resource underneath - and the layer will lend it
    // out: UnwrapUnderlyingResource hands back the ID3D12Resource and makes our
    // queue wait for the pending D3D9 work, the compositor copies from it on that
    // queue, and ReturnUnderlyingResource gives it back guarded by our fence.
    // The frame never leaves the GPU.
    class D3D12Present
    {
    public:
        // Queries the game's device for its 9on12 identity and builds the queue
        // and fence. Returns false when the client is not running on 9on12, which
        // is the caller's cue to fall back.
        bool Init(IDirect3DDevice9* gameDevice);
        void Shutdown();

        bool IsActive() const { return m_device9On12 != nullptr; }

        // Unwraps, submits both eyes to the compositor and returns the resources
        // fenced. Without stereo both eyes get 'left'.
        bool SubmitEyes(IDirect3DTexture9* left, IDirect3DTexture9* right, bool stereo);

    private:
        IDirect3DDevice9On12* m_device9On12 = nullptr;
        ID3D12Device* m_device12 = nullptr;
        ID3D12CommandQueue* m_queue = nullptr;
        ID3D12Fence* m_fence = nullptr;
        uint64_t m_fenceValue = 0;
        bool m_submitFailureLogged = false;
    };
}
