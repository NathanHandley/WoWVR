#include "proxy/real_d3d9.h"

#include "core/config.h"
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

        // A replacement library first (DXVK), if one is configured. Relative names are
        // the client folder's, never the search path's: a bare "d3d9.dll" through the
        // search path would be this proxy again.
        const std::wstring& replacement = Cfg().d3d9Library;
        if (!replacement.empty())
        {
            const bool absolute = replacement.size() > 1
                && (replacement[1] == L':' || replacement[0] == L'\\' || replacement[0] == L'/');
            const std::wstring replacementPath =
                absolute ? replacement : ModuleFile(replacement.c_str());
            g_module = LoadLibraryW(replacementPath.c_str());
            if (g_module != nullptr)
            {
                WOWVR_INFO("Loaded replacement d3d9 library %s at %p", LogWide(replacementPath.c_str()),
                           static_cast<void*>(g_module));
                return true;
            }
            WOWVR_WARN("Could not load the replacement d3d9 library %s (%s); using the "
                       "system d3d9.dll.", LogWide(replacementPath.c_str()), LogSystemError(GetLastError()));
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
