#include "game/portrait_fix.h"

#include "core/log.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace wowvr
{
    namespace
    {
        const uintptr_t kPublishedImageBase = 0x00400000u;

        // The end of the readback test at 0x00616DC0:
        //   00616E6C  8B C7   mov eax, edi      ; edi = 1 only if some alpha byte != 0xFF
        // becomes
        //   00616E6C  B0 01   mov al, 1         ; edi is 0 or 1, so eax becomes exactly 1
        const uintptr_t kSiteRva = 0x00616E6Cu - kPublishedImageBase;
        const uint8_t kExpected[2] = { 0x8Bu, 0xC7u };
        const uint8_t kPatched[2] = { 0xB0u, 0x01u };

        bool g_done = false;
    }

    bool PortraitFix::Install()
    {
        if (g_done)
        {
            return true;
        }
        g_done = true;

        uint8_t* site = reinterpret_cast<uint8_t*>(
            reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + kSiteRva);
        if (memcmp(site, kPatched, sizeof(kPatched)) == 0)
        {
            return true;
        }
        if (memcmp(site, kExpected, sizeof(kExpected)) != 0)
        {
            WOWVR_WARN("Portraits: 0x%08X does not hold the expected bytes; the client's own "
                       "portrait test is left alone.",
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(site)));
            return false;
        }

        DWORD previous = 0;
        if (!VirtualProtect(site, sizeof(kPatched), PAGE_EXECUTE_READWRITE, &previous))
        {
            WOWVR_WARN("Portraits: 0x%08X could not be made writable.",
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(site)));
            return false;
        }
        memcpy(site, kPatched, sizeof(kPatched));
        VirtualProtect(site, sizeof(kPatched), previous, &previous);
        FlushInstructionCache(GetCurrentProcess(), site, sizeof(kPatched));
        WOWVR_INFO("Portraits: kept on the render-to-texture path (test at 0x00616DC0).");
        return true;
    }

    PortraitFix& Portraits()
    {
        static PortraitFix instance;
        return instance;
    }
}
