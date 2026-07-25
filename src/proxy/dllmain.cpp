#include <windows.h>

#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "proxy/device_hooks.h"
#include "proxy/exports.h"
#include "proxy/real_d3d9.h"

namespace
{
    // WoW resolves Direct3DCreate9 with LoadLibrary/GetProcAddress and then frees
    // the module again, keeping only the IDirect3D9 interface. That would unload us
    // moments after startup, so pin ourselves for the life of the process.
    void PinSelf()
    {
        HMODULE pinned = nullptr;
        const BOOL ok = GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&PinSelf), &pinned);

        if (!ok)
        {
            WOWVR_WARN("Could not pin the proxy in memory: %s. WoWVR may be unloaded "
                       "before it can wrap the device.", wowvr::LogSystemError(GetLastError()));
        }
    }

    bool AttachProcess(HMODULE self)
    {
        wowvr::InitPaths(self);
        wowvr::LoadConfig();
        wowvr::LogInit(wowvr::ModuleFile(L"WoWVR.log").c_str(), wowvr::Cfg().logLevel);

        WOWVR_INFO("WoWVR proxy attaching to %s", wowvr::LogWide(wowvr::HostProcessName().c_str()));
        WOWVR_INFO("Module directory: %s", wowvr::LogWide(wowvr::ModuleDirectory().c_str()));
        WOWVR_INFO("Forwarding to:    %s", wowvr::LogWide(wowvr::SystemD3D9Path().c_str()));
        WOWVR_INFO("Config: enabled=%d vr=%d flatDebug=%d renderScale=%.2f headTracking=%d",
                   wowvr::Cfg().enabled ? 1 : 0,
                   wowvr::Cfg().vrEnabled ? 1 : 0,
                   wowvr::Cfg().flatDebug ? 1 : 0,
                   wowvr::Cfg().renderScale,
                   wowvr::Cfg().headTracking ? 1 : 0);

        // Loading a system DLL from DllMain is the standard proxy pattern and is
        // safe here: d3d9.dll pulls in nothing that re-enters our loader lock.
        if (!wowvr::InitProxyExports())
        {
            WOWVR_ERROR("Could not wire up the system d3d9.dll; failing the load so the "
                        "problem is obvious rather than silent.");
            return false;
        }

        PinSelf();
        return true;
    }

    void DetachProcess()
    {
        WOWVR_INFO("WoWVR proxy detaching.");
        wowvr::ShutdownRenderer();
        wowvr::UnloadRealD3D9();
        wowvr::LogShutdown();
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        return AttachProcess(module) ? TRUE : FALSE;

    case DLL_PROCESS_DETACH:
        DetachProcess();
        break;

    default:
        break;
    }

    return TRUE;
}
