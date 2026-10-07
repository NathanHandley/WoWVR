#include "game/interior_view.h"

#include "core/log.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace wowvr
{
    namespace
    {
        const uintptr_t kPublishedImageBase = 0x00400000u;
        const uintptr_t kEyeRva = 0x00CD8F5Cu - kPublishedImageBase;

        // 0x00795D7F mov eax,[eye.x]; 0x00795D84 mov edx,[eye.z]; 0x00795D8A mov ecx,[eye.y]
        // - the only reads of the eye in the "which building is the camera in" test.
        const uintptr_t kLoadsRva = 0x00795D7Fu - kPublishedImageBase;
        const uint8_t kLoadsBytes[17] = {
            0xA1u, 0x5Cu, 0x8Fu, 0xCDu, 0x00u,          // mov eax, [0x00CD8F5C]
            0x8Bu, 0x15u, 0x64u, 0x8Fu, 0xCDu, 0x00u,   // mov edx, [0x00CD8F64]
            0x8Bu, 0x0Du, 0x60u, 0x8Fu, 0xCDu, 0x00u,   // mov ecx, [0x00CD8F60]
        };
        // Where each displacement sits in those bytes, and which component it reads.
        const int kDispOffset[3] = { 1, 7, 13 };
        const int kComponent[3] = { 0, 2, 1 };

        // ClntObjMgrGetActivePlayerObj: the local player's object or null.
        const uintptr_t kActivePlayerObjectRva = 0x004038F0u - kPublishedImageBase;
        typedef void*(__cdecl* ActivePlayerObjectFn)();
        const uintptr_t kUnitPosition = 0x798u;   // x, y, z

        // A yard above the feet: inside the group the character stands in, below any
        // ceiling, so the client's downward ray lands on that group's floor.
        const float kAboveFeet = 1.0f;

        // Read by the patched instructions - a plain global so its address never moves.
        float g_decisionEye[3] = {};

        uintptr_t ImageBase()
        {
            return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        }

        bool CharacterPosition(float out[3])
        {
            __try
            {
                void* player = reinterpret_cast<ActivePlayerObjectFn>(
                    ImageBase() + kActivePlayerObjectRva)();
                if (player == nullptr)
                {
                    return false;
                }
                memcpy(out, reinterpret_cast<const uint8_t*>(player) + kUnitPosition,
                       3 * sizeof(float));
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
        }
    }

    bool InteriorView::Install()
    {
        if (m_installed) { return true; }
        if (m_failed) { return false; }

        uint8_t* site = reinterpret_cast<uint8_t*>(ImageBase() + kLoadsRva);
        if (memcmp(site, kLoadsBytes, sizeof(kLoadsBytes)) != 0)
        {
            m_failed = true;
            WOWVR_WARN("Interior view: 0x00795D7F does not hold the expected eye loads; "
                       "building interiors stay decided by the camera.");
            return false;
        }

        // Seeded before the first read can reach it.
        memcpy(g_decisionEye, reinterpret_cast<const void*>(ImageBase() + kEyeRva),
               sizeof(g_decisionEye));

        DWORD previous = 0;
        if (!VirtualProtect(site, sizeof(kLoadsBytes), PAGE_EXECUTE_READWRITE, &previous))
        {
            m_failed = true;
            WOWVR_WARN("Interior view: 0x00795D7F could not be made writable.");
            return false;
        }
        for (int i = 0; i < 3; ++i)
        {
            const uint32_t address = static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(&g_decisionEye[kComponent[i]]));
            memcpy(site + kDispOffset[i], &address, 4);
        }
        VirtualProtect(site, sizeof(kLoadsBytes), previous, &previous);
        FlushInstructionCache(GetCurrentProcess(), site, sizeof(kLoadsBytes));
        m_installed = true;
        WOWVR_INFO("Interior view: the 'which building is the camera in' test (0x00795D40) "
                   "now reads its position from WoWVR.");
        return true;
    }

    void InteriorView::Update(bool fromCharacter)
    {
        if (!m_installed)
        {
            return;
        }
        float position[3];
        const bool character = fromCharacter && CharacterPosition(position);
        if (character)
        {
            g_decisionEye[0] = position[0];
            g_decisionEye[1] = position[1];
            g_decisionEye[2] = position[2] + kAboveFeet;
        }
        else
        {
            // The camera, as stock (a frame old, which a stationary building does not mind).
            memcpy(g_decisionEye, reinterpret_cast<const void*>(ImageBase() + kEyeRva),
                   sizeof(g_decisionEye));
        }
        if (character != m_lastFromCharacter)
        {
            m_lastFromCharacter = character;
            WOWVR_INFO("Interior view: buildings are entered from the %s.",
                       character ? "character's position" : "camera's position");
        }
    }

    InteriorView& Interior()
    {
        static InteriorView instance;
        return instance;
    }
}
