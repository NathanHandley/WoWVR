#include "proxy/real_d3d9.h"

#include "core/log.h"
#include "core/paths.h"

namespace wowvr
{
    namespace
    {
        HMODULE g_module = nullptr;
    }

    bool LoadRealD3D9()
    {
        if (g_module != nullptr)
        {
            return true;
        }

        const std::wstring& path = SystemD3D9Path();
        g_module = LoadLibraryW(path.c_str());
        if (g_module == nullptr)
        {
            WOWVR_ERROR("Failed to load the system d3d9.dll: %s", LogSystemError(GetLastError()));
            return false;
        }

        WOWVR_INFO("Loaded system d3d9.dll at %p", static_cast<void*>(g_module));
        return true;
    }

    void UnloadRealD3D9()
    {
        // Deliberately not FreeLibrary'd. We are usually unloading during process
        // teardown and the graphics driver may still hold references.
        g_module = nullptr;
    }

    HMODULE RealD3D9Module()
    {
        return g_module;
    }

    FARPROC RealProc(const char* name)
    {
        if (g_module == nullptr)
        {
            return nullptr;
        }
        FARPROC proc = GetProcAddress(g_module, name);
        if (proc == nullptr)
        {
            WOWVR_WARN("System d3d9.dll has no export '%s'", name);
        }
        return proc;
    }

    FARPROC RealProcByOrdinal(WORD ordinal)
    {
        if (g_module == nullptr)
        {
            return nullptr;
        }
        FARPROC proc = GetProcAddress(g_module, MAKEINTRESOURCEA(ordinal));
        if (proc == nullptr)
        {
            WOWVR_DEBUG("System d3d9.dll has no export at ordinal %u", ordinal);
        }
        return proc;
    }
}
