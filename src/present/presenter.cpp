#include "present/presenter.h"

#include "core/image_dump.h"
#include "core/log.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <dxgi.h>

#include <openvr.h>

namespace wowvr
{
    namespace
    {
        // A slice of a row-by-row copy, for spreading one big frame copy across
        // cores. Staging memory is write-combined, where a single stream tops out
        // around 3-4 GB/s; a few parallel streams roughly triple that, which is
        // most of the difference between a 10 ms upload and a 3 ms one.
        struct RowCopySlice
        {
            uint8_t* destination;
            const uint8_t* source;
            uint32_t destinationPitch;
            uint32_t sourcePitch;
            uint32_t rowBytes;
            uint32_t rowBegin;
            uint32_t rowEnd;
        };

        void CopyRowSlice(RowCopySlice slice)
        {
            for (uint32_t row = slice.rowBegin; row < slice.rowEnd; ++row)
            {
                std::memcpy(slice.destination + static_cast<size_t>(row) * slice.destinationPitch,
                            slice.source + static_cast<size_t>(row) * slice.sourcePitch,
                            slice.rowBytes);
            }
        }

        // The slices of one dispatch, claimed by pool workers with an interlocked
        // index. Upload is only ever called from the game's render thread, so a
        // single static dispatch state is enough.
        struct RowCopyDispatch
        {
            RowCopySlice slices[6];
            LONG nextSlice = 0;
            LONG remaining = 0;
            HANDLE doneEvent = nullptr;
        };

        RowCopyDispatch g_rowCopyDispatch;
        PTP_WORK g_rowCopyWork = nullptr;
        bool g_rowCopyPoolBroken = false;

        VOID CALLBACK RowCopyWorkCallback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_WORK)
        {
            RowCopyDispatch* dispatch = static_cast<RowCopyDispatch*>(context);
            const LONG index = InterlockedIncrement(&dispatch->nextSlice) - 1;
            CopyRowSlice(dispatch->slices[index]);
            if (InterlockedDecrement(&dispatch->remaining) == 0)
            {
                SetEvent(dispatch->doneEvent);
            }
        }

        void CopyRowsParallel(void* destination, uint32_t destinationPitch,
                              const void* source, uint32_t sourcePitch,
                              uint32_t rowBytes, uint32_t rows)
        {
            unsigned sliceCount = std::thread::hardware_concurrency() / 2;
            sliceCount = std::clamp(sliceCount, 2u, 6u);
            sliceCount = std::min(sliceCount, rows);

            // The workers come from the process's kernel thread pool: no
            // per-frame thread creation (the old std::thread version paid up to
            // ten spawn/joins a frame across both eyes), and no threads of our
            // own to tear down at DLL unload. Created once, never closed - the
            // pool must outlive every Upload, and the process reclaims it.
            if (g_rowCopyWork == nullptr && !g_rowCopyPoolBroken && sliceCount > 1)
            {
                g_rowCopyDispatch.doneEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (g_rowCopyDispatch.doneEvent != nullptr)
                {
                    g_rowCopyWork = CreateThreadpoolWork(RowCopyWorkCallback,
                                                         &g_rowCopyDispatch, nullptr);
                }
                if (g_rowCopyWork == nullptr)
                {
                    WOWVR_WARN("Thread pool unavailable (%lu); frame copies run "
                               "single-threaded.", GetLastError());
                    g_rowCopyPoolBroken = true;
                }
            }

            if (g_rowCopyWork == nullptr)
            {
                sliceCount = 1;
            }

            const uint32_t rowsPerSlice = (rows + sliceCount - 1) / sliceCount;

            g_rowCopyDispatch.nextSlice = 0;
            g_rowCopyDispatch.remaining = static_cast<LONG>(sliceCount) - 1;

            RowCopySlice callerSlice = {};
            for (unsigned i = 0; i < sliceCount; ++i)
            {
                RowCopySlice slice;
                slice.destination = static_cast<uint8_t*>(destination);
                slice.source = static_cast<const uint8_t*>(source);
                slice.destinationPitch = destinationPitch;
                slice.sourcePitch = sourcePitch;
                slice.rowBytes = rowBytes;
                slice.rowBegin = i * rowsPerSlice;
                slice.rowEnd = std::min(rows, slice.rowBegin + rowsPerSlice);

                if (i + 1 == sliceCount)
                {
                    callerSlice = slice;  // the calling thread takes the last slice
                }
                else
                {
                    g_rowCopyDispatch.slices[i] = slice;
                }
            }

            for (unsigned i = 0; i + 1 < sliceCount; ++i)
            {
                SubmitThreadpoolWork(g_rowCopyWork);
            }

            CopyRowSlice(callerSlice);

            if (sliceCount > 1)
            {
                WaitForSingleObject(g_rowCopyDispatch.doneEvent, INFINITE);
            }
        }
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

        WOWVR_INFO("Presenter ready: D3D11 feature level 0x%04x, %ux%u per eye.",
                   static_cast<unsigned>(achievedLevel), m_width, m_height);
        return true;
    }

    bool Presenter::CreateEyeTextures()
    {
        if (m_device == nullptr)
        {
            return false;
        }

        ReleaseTextures();

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

        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        stagingDesc.MiscFlags = 0;

        for (int eye = 0; eye < 2; ++eye)
        {
            const HRESULT textureResult = m_device->CreateTexture2D(&desc, nullptr, &m_eyeTexture[eye]);
            if (FAILED(textureResult))
            {
                WOWVR_ERROR("Could not create the %s eye texture (0x%08lx).",
                            eye == 0 ? "left" : "right", textureResult);
                ReleaseTextures();
                return false;
            }

            // The staging half of the upload. Optional: without it Upload falls
            // back to UpdateSubresource, which is correct but single-threaded.
            if (FAILED(m_device->CreateTexture2D(&stagingDesc, nullptr, &m_stagingTexture[eye])))
            {
                m_stagingTexture[eye] = nullptr;
            }
        }

        return true;
    }

    bool Presenter::AdoptSharedTextures(void* leftHandle, void* rightHandle)
    {
        if (m_device == nullptr)
        {
            return false;
        }

        ReleaseTextures();

        void* handles[2] = { leftHandle, rightHandle };
        for (int eye = 0; eye < 2; ++eye)
        {
            HRESULT hr = E_INVALIDARG;
            if (handles[eye] != nullptr)
            {
                hr = m_device->OpenSharedResource(
                    static_cast<HANDLE>(handles[eye]), __uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(&m_eyeTexture[eye]));
            }
            if (FAILED(hr) || m_eyeTexture[eye] == nullptr)
            {
                WOWVR_WARN("Opening the %s shared eye surface in D3D11 failed (0x%08lx).",
                           eye == 0 ? "left" : "right", hr);
                ReleaseTextures();
                return false;
            }
        }

        m_adopted = true;
        WOWVR_INFO("Presenter adopted the shared eye surfaces; frames stay on the GPU (zero-copy).");
        return true;
    }

    void Presenter::ReleaseAdoptedTextures()
    {
        if (m_adopted)
        {
            ReleaseTextures();
        }
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
            if (m_stagingTexture[eye] != nullptr)
            {
                m_stagingTexture[eye]->Release();
                m_stagingTexture[eye] = nullptr;
            }
        }
        if (m_overlayTexture != nullptr)
        {
            m_overlayTexture->Release();
            m_overlayTexture = nullptr;
        }
        if (m_overlayStaging != nullptr)
        {
            m_overlayStaging->Release();
            m_overlayStaging = nullptr;
        }
        m_overlayWidth = 0;
        m_overlayHeight = 0;
        m_adopted = false;
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

    bool Presenter::UploadOverlay(const void* pixels, uint32_t sourceRowPitch, uint32_t width,
                                  uint32_t height)
    {
        if (m_device == nullptr || m_context == nullptr || pixels == nullptr || width == 0
            || height == 0)
        {
            return false;
        }

        if (m_overlayTexture == nullptr || m_overlayWidth != width || m_overlayHeight != height)
        {
            if (m_overlayTexture != nullptr) { m_overlayTexture->Release(); m_overlayTexture = nullptr; }
            if (m_overlayStaging != nullptr) { m_overlayStaging->Release(); m_overlayStaging = nullptr; }

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // D3DFMT_A8R8G8B8 byte for byte
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_overlayTexture);
            if (FAILED(hr))
            {
                m_overlayTexture = nullptr;
                WOWVR_ERROR("Could not create the %ux%u interface overlay texture (0x%08lx).",
                            width, height, hr);
                return false;
            }

            D3D11_TEXTURE2D_DESC stagingDesc = desc;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(m_device->CreateTexture2D(&stagingDesc, nullptr, &m_overlayStaging)))
            {
                m_overlayStaging = nullptr;
            }
            m_overlayWidth = width;
            m_overlayHeight = height;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (m_overlayStaging != nullptr
            && SUCCEEDED(m_context->Map(m_overlayStaging, 0, D3D11_MAP_WRITE, 0, &mapped)))
        {
            CopyRowsParallel(mapped.pData, mapped.RowPitch, pixels, sourceRowPitch,
                             width * 4, height);
            m_context->Unmap(m_overlayStaging, 0);
            m_context->CopyResource(m_overlayTexture, m_overlayStaging);
            return true;
        }

        m_context->UpdateSubresource(m_overlayTexture, 0, nullptr, pixels, sourceRowPitch, 0);
        return true;
    }

    bool Presenter::DumpCompositorMirror(const wchar_t* path)
    {
        if (m_device == nullptr || m_context == nullptr || vr::VRCompositor() == nullptr)
        {
            return false;
        }
        ID3D11ShaderResourceView* view = nullptr;
        const vr::EVRCompositorError error = vr::VRCompositor()->GetMirrorTextureD3D11(
            vr::Eye_Left, m_device, reinterpret_cast<void**>(&view));
        if (error != vr::VRCompositorError_None || view == nullptr)
        {
            WOWVR_WARN("Compositor mirror unavailable (%d).", static_cast<int>(error));
            return false;
        }

        bool written = false;
        ID3D11Resource* resource = nullptr;
        view->GetResource(&resource);
        ID3D11Texture2D* texture = nullptr;
        if (resource != nullptr
            && SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                                  reinterpret_cast<void**>(&texture))))
        {
            D3D11_TEXTURE2D_DESC desc = {};
            texture->GetDesc(&desc);
            D3D11_TEXTURE2D_DESC stagingDesc = desc;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stagingDesc.MiscFlags = 0;
            stagingDesc.MipLevels = 1;
            stagingDesc.ArraySize = 1;
            stagingDesc.SampleDesc.Count = 1;
            ID3D11Texture2D* staging = nullptr;
            if (SUCCEEDED(m_device->CreateTexture2D(&stagingDesc, nullptr, &staging)))
            {
                m_context->CopySubresourceRegion(staging, 0, 0, 0, 0, texture, 0, nullptr);
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                if (SUCCEEDED(m_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
                {
                    // RGBA formats are swizzled to the BGRA the BMP writer expects.
                    const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM
                        || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                        || desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
                    std::vector<uint8_t> pixels(static_cast<size_t>(desc.Width) * desc.Height * 4);
                    for (uint32_t y = 0; y < desc.Height; ++y)
                    {
                        const uint8_t* row = static_cast<const uint8_t*>(mapped.pData)
                                             + static_cast<size_t>(y) * mapped.RowPitch;
                        uint8_t* out = pixels.data() + static_cast<size_t>(y) * desc.Width * 4;
                        for (uint32_t x = 0; x < desc.Width; ++x)
                        {
                            out[x * 4 + 0] = row[x * 4 + (rgba ? 2 : 0)];
                            out[x * 4 + 1] = row[x * 4 + 1];
                            out[x * 4 + 2] = row[x * 4 + (rgba ? 0 : 2)];
                            out[x * 4 + 3] = row[x * 4 + 3];
                        }
                    }
                    m_context->Unmap(staging, 0);
                    written = SaveBgraBmp(path, pixels.data(), desc.Width, desc.Height,
                                          desc.Width * 4);
                    WOWVR_INFO("Compositor mirror %ux%u (format %d) written.", desc.Width,
                               desc.Height, static_cast<int>(desc.Format));
                }
                staging->Release();
            }
            texture->Release();
        }
        if (resource != nullptr)
        {
            resource->Release();
        }
        vr::VRCompositor()->ReleaseMirrorTextureD3D11(view);
        return written;
    }

    bool Presenter::Upload(int eye, const void* pixels, uint32_t sourceRowPitch)
    {
        if (m_context == nullptr || eye < 0 || eye > 1 || m_eyeTexture[eye] == nullptr || pixels == nullptr)
        {
            return false;
        }

        // Map + parallel copy + GPU-side CopyResource. The map can stall on the
        // previous frame's CopyResource, but that copy is a DMA measured in a
        // millisecond or two against a 11 ms frame, so in practice it is idle.
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (m_stagingTexture[eye] != nullptr
            && SUCCEEDED(m_context->Map(m_stagingTexture[eye], 0, D3D11_MAP_WRITE, 0, &mapped)))
        {
            CopyRowsParallel(mapped.pData, mapped.RowPitch, pixels, sourceRowPitch,
                             m_width * 4, m_height);
            m_context->Unmap(m_stagingTexture[eye], 0);
            m_context->CopyResource(m_eyeTexture[eye], m_stagingTexture[eye]);
            return true;
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
