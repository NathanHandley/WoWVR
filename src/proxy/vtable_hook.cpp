#include "proxy/vtable_hook.h"

#include "core/log.h"

#include <windows.h>

namespace wowvr
{
    void** VTableOf(void* comObject)
    {
        if (comObject == nullptr)
        {
            return nullptr;
        }
        return *reinterpret_cast<void***>(comObject);
    }

    bool HookVTableSlot(void* comObject, unsigned slot, void* replacement, void** originalOut)
    {
        if (originalOut != nullptr)
        {
            *originalOut = nullptr;
        }

        void** vtable = VTableOf(comObject);
        if (vtable == nullptr || replacement == nullptr)
        {
            return false;
        }

        void* existing = vtable[slot];
        if (existing == replacement)
        {
            // Already hooked, most likely because a second device was created.
            return true;
        }

        DWORD previousProtection = 0;
        if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &previousProtection))
        {
            WOWVR_ERROR("VirtualProtect failed for vtable slot %u: %s",
                        slot, LogSystemError(GetLastError()));
            return false;
        }

        vtable[slot] = replacement;

        DWORD restored = 0;
        VirtualProtect(&vtable[slot], sizeof(void*), previousProtection, &restored);
        FlushInstructionCache(GetCurrentProcess(), &vtable[slot], sizeof(void*));

        if (originalOut != nullptr)
        {
            *originalOut = existing;
        }

        WOWVR_DEBUG("Hooked vtable slot %u: %p -> %p", slot, existing, replacement);
        return true;
    }
}
