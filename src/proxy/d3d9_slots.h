#pragma once

// COM vtable slot numbers for the D3D9 interfaces we hook.
//
// These were generated from the declaration order in the Windows SDK header
// (Include/10.0.26100.0/shared/d3d9.h) rather than written from memory. The D3D9
// interfaces have been frozen since 2002, so they are stable, but if anything here
// ever looks suspect the check is simply the order of STDMETHOD declarations
// inside DECLARE_INTERFACE_(IDirect3DDevice9, IUnknown).

namespace wowvr
{
    namespace slot
    {
        namespace d3d9
        {
            constexpr unsigned QueryInterface = 0;
            constexpr unsigned AddRef = 1;
            constexpr unsigned Release = 2;
            constexpr unsigned CreateDevice = 16;
            constexpr unsigned Count = 17;
        }

        namespace device9
        {
            constexpr unsigned QueryInterface = 0;
            constexpr unsigned AddRef = 1;
            constexpr unsigned Release = 2;
            constexpr unsigned Reset = 16;
            constexpr unsigned Present = 17;
            constexpr unsigned CreateRenderTarget = 28;
            constexpr unsigned CreateDepthStencilSurface = 29;
            constexpr unsigned StretchRect = 34;
            constexpr unsigned SetRenderTarget = 37;
            constexpr unsigned GetRenderTarget = 38;
            constexpr unsigned GetDepthStencilSurface = 40;
            constexpr unsigned SetDepthStencilSurface = 39;
            constexpr unsigned BeginScene = 41;
            constexpr unsigned EndScene = 42;
            constexpr unsigned Clear = 43;
            constexpr unsigned SetTransform = 44;
            constexpr unsigned SetViewport = 47;
            constexpr unsigned SetRenderState = 57;
            constexpr unsigned SetScissorRect = 75;
            constexpr unsigned DrawPrimitive = 81;
            constexpr unsigned DrawIndexedPrimitive = 82;
            constexpr unsigned DrawPrimitiveUP = 83;
            constexpr unsigned DrawIndexedPrimitiveUP = 84;
            constexpr unsigned SetVertexDeclaration = 87;
            constexpr unsigned SetFVF = 89;
            constexpr unsigned CreateVertexShader = 91;
            constexpr unsigned SetVertexShader = 92;
            constexpr unsigned SetVertexShaderConstantF = 94;
            constexpr unsigned CreatePixelShader = 106;
            constexpr unsigned SetPixelShader = 107;
            constexpr unsigned Count = 119;
        }
    }
}
