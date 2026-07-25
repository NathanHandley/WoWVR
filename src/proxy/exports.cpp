// The d3d9.dll export surface.
//
// Everything the real system d3d9.dll exports has to be exported here too,
// otherwise anything else in the process that resolves a d3d9 symbol falls over.
// Only the two device factories are genuinely implemented; the rest are naked
// tail-jumps straight into the system DLL, which keeps us out of the business of
// having to know their calling conventions or argument counts.

#include "proxy/exports.h"

#include "core/config.h"
#include "core/log.h"
#include "proxy/device_hooks.h"
#include "proxy/real_d3d9.h"

#include <d3d9.h>

namespace
{
    IDirect3D9* (WINAPI *g_realDirect3DCreate9)(UINT) = nullptr;
    HRESULT     (WINAPI *g_realDirect3DCreate9Ex)(UINT, IDirect3D9Ex**) = nullptr;
}

// ---------------------------------------------------------------------------
// Pass-through thunks
// ---------------------------------------------------------------------------
#define WOWVR_THUNK(exportName)                                       \
    FARPROC g_real_##exportName = nullptr;                            \
    extern "C" __declspec(naked) void WoWVR_##exportName()            \
    {                                                                 \
        __asm { jmp dword ptr [g_real_##exportName] }                 \
    }

WOWVR_THUNK(D3DPERF_BeginEvent)
WOWVR_THUNK(D3DPERF_EndEvent)
WOWVR_THUNK(D3DPERF_GetStatus)
WOWVR_THUNK(D3DPERF_QueryRepeatFrame)
WOWVR_THUNK(D3DPERF_SetMarker)
WOWVR_THUNK(D3DPERF_SetOptions)
WOWVR_THUNK(D3DPERF_SetRegion)
WOWVR_THUNK(DebugSetLevel)
WOWVR_THUNK(DebugSetMute)
WOWVR_THUNK(Direct3D9EnableMaximizedWindowedModeShim)
WOWVR_THUNK(Direct3DCreate9On12)
WOWVR_THUNK(Direct3DCreate9On12Ex)
WOWVR_THUNK(Direct3DShaderValidatorCreate9)
WOWVR_THUNK(PSGPError)
WOWVR_THUNK(PSGPSampleTexture)

// d3d9.dll also exports a handful of undocumented ordinal-only entry points.
// Nothing we care about calls them, but a proxy that drops exports is a proxy
// that breaks somebody's overlay, so they are forwarded as well.
WOWVR_THUNK(Ordinal16)
WOWVR_THUNK(Ordinal17)
WOWVR_THUNK(Ordinal18)
WOWVR_THUNK(Ordinal19)
WOWVR_THUNK(Ordinal22)
WOWVR_THUNK(Ordinal23)

#undef WOWVR_THUNK

namespace wowvr
{
    namespace
    {
        void LogAdapterSummary(IDirect3D9* d3d9)
        {
            if (d3d9 == nullptr)
            {
                return;
            }

            const UINT adapterCount = d3d9->GetAdapterCount();
            WOWVR_INFO("Direct3D9 reports %u adapter(s)", adapterCount);

            for (UINT adapter = 0; adapter < adapterCount; ++adapter)
            {
                D3DADAPTER_IDENTIFIER9 identifier = {};
                if (FAILED(d3d9->GetAdapterIdentifier(adapter, 0, &identifier)))
                {
                    continue;
                }

                D3DDISPLAYMODE mode = {};
                d3d9->GetAdapterDisplayMode(adapter, &mode);

                WOWVR_INFO("  adapter %u: %s (%s) driver %s - desktop %ux%u @ %uHz",
                           adapter, identifier.Description, identifier.DeviceName,
                           identifier.Driver, mode.Width, mode.Height, mode.RefreshRate);
            }
        }
    }

    bool InitProxyExports()
    {
        if (!LoadRealD3D9())
        {
            return false;
        }

        g_realDirect3DCreate9 =
            reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(RealProc("Direct3DCreate9"));
        g_realDirect3DCreate9Ex =
            reinterpret_cast<HRESULT (WINAPI*)(UINT, IDirect3D9Ex**)>(RealProc("Direct3DCreate9Ex"));

        if (g_realDirect3DCreate9 == nullptr)
        {
            WOWVR_ERROR("System d3d9.dll does not export Direct3DCreate9; refusing to load.");
            return false;
        }

        g_real_D3DPERF_BeginEvent       = RealProc("D3DPERF_BeginEvent");
        g_real_D3DPERF_EndEvent         = RealProc("D3DPERF_EndEvent");
        g_real_D3DPERF_GetStatus        = RealProc("D3DPERF_GetStatus");
        g_real_D3DPERF_QueryRepeatFrame = RealProc("D3DPERF_QueryRepeatFrame");
        g_real_D3DPERF_SetMarker        = RealProc("D3DPERF_SetMarker");
        g_real_D3DPERF_SetOptions       = RealProc("D3DPERF_SetOptions");
        g_real_D3DPERF_SetRegion        = RealProc("D3DPERF_SetRegion");
        g_real_DebugSetLevel            = RealProc("DebugSetLevel");
        g_real_DebugSetMute             = RealProc("DebugSetMute");
        g_real_Direct3D9EnableMaximizedWindowedModeShim =
            RealProc("Direct3D9EnableMaximizedWindowedModeShim");
        g_real_Direct3DCreate9On12      = RealProc("Direct3DCreate9On12");
        g_real_Direct3DCreate9On12Ex    = RealProc("Direct3DCreate9On12Ex");
        g_real_Direct3DShaderValidatorCreate9 = RealProc("Direct3DShaderValidatorCreate9");
        g_real_PSGPError                = RealProc("PSGPError");
        g_real_PSGPSampleTexture        = RealProc("PSGPSampleTexture");

        g_real_Ordinal16 = RealProcByOrdinal(16);
        g_real_Ordinal17 = RealProcByOrdinal(17);
        g_real_Ordinal18 = RealProcByOrdinal(18);
        g_real_Ordinal19 = RealProcByOrdinal(19);
        g_real_Ordinal22 = RealProcByOrdinal(22);
        g_real_Ordinal23 = RealProcByOrdinal(23);

        return true;
    }
}

// ---------------------------------------------------------------------------
// The entry points we actually care about
// ---------------------------------------------------------------------------
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion)
{
    if (g_realDirect3DCreate9 == nullptr)
    {
        return nullptr;
    }

    IDirect3D9* real = g_realDirect3DCreate9(SDKVersion);
    WOWVR_INFO("Direct3DCreate9(SDKVersion=%u) -> %p", SDKVersion, static_cast<void*>(real));

    if (real == nullptr)
    {
        return nullptr;
    }

    if (!wowvr::Cfg().enabled)
    {
        WOWVR_INFO("WoWVR is disabled in WoWVR.ini; handing back the unwrapped interface.");
        return real;
    }

    wowvr::LogAdapterSummary(real);
    wowvr::InstallD3D9Hooks(real);
    return real;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D)
{
    if (g_realDirect3DCreate9Ex == nullptr)
    {
        return E_NOTIMPL;
    }

    const HRESULT hr = g_realDirect3DCreate9Ex(SDKVersion, ppD3D);
    WOWVR_INFO("Direct3DCreate9Ex(SDKVersion=%u) -> 0x%08lx, %p",
               SDKVersion, hr, ppD3D != nullptr ? static_cast<void*>(*ppD3D) : nullptr);
    return hr;
}
