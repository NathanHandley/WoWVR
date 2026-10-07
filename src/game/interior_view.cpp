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

        // The one call to the decision, in the world update: retargeted to the wrapper
        // below so the decision can be made from the camera first and the character second.
        const uintptr_t kDecisionCallRva = 0x00783381u - kPublishedImageBase;
        const uintptr_t kDecisionRva = 0x00795D40u - kPublishedImageBase;
        typedef void(__cdecl* DecisionFn)();

        // What the decision found: the building(s) the eye is in, zero when none.
        const uintptr_t kInsideMapObjRva = 0x00CD87A4u - kPublishedImageBase;
        const uintptr_t kInsideMapObj2Rva = 0x00CD87A0u - kPublishedImageBase;

        // Read by the patched instructions - a plain global so its address never moves.
        float g_decisionEye[3] = {};

        DecisionFn g_decision = nullptr;
        bool g_fromCharacter = false;
        float g_cameraReach = 10.0f;     // yards; see DecisionWrapper
        bool g_usedCharacter = false;   // what the last decision ended up using

        uintptr_t ImageBase()
        {
            return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        }

        bool CharacterPosition(float out[3]);

        // The start-group lists the decision fills, one per building slot: capacity,
        // count, data (uint32 group indices), grow step - appended to by 0x00792FC0,
        // which skips duplicates and grows the array the client's own way.
        const uintptr_t kGroupListRva = 0x00CDB0D4u - kPublishedImageBase;
        const uintptr_t kGroupList2Rva = 0x00CDB0E4u - kPublishedImageBase;
        const uintptr_t kAppendGroupRva = 0x00792FC0u - kPublishedImageBase;
        typedef void(__cdecl* AppendGroupFn)(void* list, uint32_t group);
        const int kMaxCarried = 16;

        struct GroupList
        {
            uint32_t capacity;
            uint32_t count;
            uint32_t* data;
            uint32_t grow;
        };

        // One building slot's answer: the building and the groups the eye is in.
        struct SlotResult
        {
            uint32_t mapObj;
            int count;
            uint32_t groups[kMaxCarried];
        };

        void SetEye(const float eye[3])
        {
            memcpy(g_decisionEye, eye, sizeof(g_decisionEye));
        }

        void ReadSlot(uintptr_t mapObjRva, uintptr_t listRva, SlotResult& out)
        {
            const uintptr_t base = ImageBase();
            out.mapObj = *reinterpret_cast<const uint32_t*>(base + mapObjRva);
            out.count = 0;
            const GroupList* list = reinterpret_cast<const GroupList*>(base + listRva);
            if (out.mapObj == 0 || list->data == nullptr)
            {
                return;
            }
            for (uint32_t i = 0; i < list->count && out.count < kMaxCarried; ++i)
            {
                out.groups[out.count++] = list->data[i];
            }
        }

        // Adds a slot's groups to whichever of the camera's slots holds the same building,
        // or to the free second slot. True if anything was added.
        bool MergeSlot(const SlotResult& from)
        {
            if (from.mapObj == 0 || from.count == 0)
            {
                return false;
            }
            const uintptr_t base = ImageBase();
            uint32_t* slot1 = reinterpret_cast<uint32_t*>(base + kInsideMapObjRva);
            uint32_t* slot2 = reinterpret_cast<uint32_t*>(base + kInsideMapObj2Rva);
            void* list = nullptr;
            if (*slot1 == from.mapObj || *slot1 == 0)
            {
                *slot1 = from.mapObj;
                list = reinterpret_cast<void*>(base + kGroupListRva);
            }
            else if (*slot2 == from.mapObj || *slot2 == 0)
            {
                *slot2 = from.mapObj;
                list = reinterpret_cast<void*>(base + kGroupList2Rva);
            }
            else
            {
                return false;   // the camera already fills both slots with other buildings
            }
            const GroupList* before = static_cast<const GroupList*>(list);
            const uint32_t countBefore = before->count;
            const AppendGroupFn append = reinterpret_cast<AppendGroupFn>(base + kAppendGroupRva);
            for (int i = 0; i < from.count; ++i)
            {
                append(list, from.groups[i]);
            }
            return before->count != countBefore;
        }

        // Near the character, both: the walk starts from the groups around the camera
        // AND the ones around the character. The camera's decision runs last, so
        // everything it sets - its slots, its lists, its lighting side effects - is stock,
        // and the character's groups are then added to its lists. Starting from the
        // character alone culled the room the camera stood in whenever the two were
        // different groups (the camera a step back in the passage behind them).
        //
        // Beyond g_cameraReach, the character's alone: zoomed out into another tunnel, the
        // camera's own groups are drawn between it and the character and their walls hide
        // the character's room completely, so the camera's are dropped and the room is
        // seen through the rock instead. Either way, if the character is in no building,
        // the camera's own decision stands.
        void __cdecl DecisionWrapper()
        {
            const uintptr_t base = ImageBase();
            float camera[3];
            memcpy(camera, reinterpret_cast<const void*>(base + kEyeRva), sizeof(camera));
            float position[3];
            if (!g_fromCharacter || !CharacterPosition(position))
            {
                SetEye(camera);
                g_decision();
                g_usedCharacter = false;
                return;
            }

            const float character[3] = { position[0], position[1], position[2] + kAboveFeet };
            SetEye(character);
            g_decision();
            SlotResult first;
            SlotResult second;
            ReadSlot(kInsideMapObjRva, kGroupListRva, first);
            ReadSlot(kInsideMapObj2Rva, kGroupList2Rva, second);
            if (first.mapObj == 0 && second.mapObj == 0)
            {
                SetEye(camera);       // the character is outdoors: stock
                g_decision();
                g_usedCharacter = false;
                return;
            }
            const float dx = camera[0] - character[0];
            const float dy = camera[1] - character[1];
            const float dz = camera[2] - character[2];
            if (dx * dx + dy * dy + dz * dz > g_cameraReach * g_cameraReach)
            {
                g_usedCharacter = true;   // the character's decision stands alone
                return;
            }

            SetEye(camera);
            g_decision();
            const bool added1 = MergeSlot(first);
            const bool added2 = MergeSlot(second);
            g_usedCharacter = added1 || added2;
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
        uint8_t* call = reinterpret_cast<uint8_t*>(ImageBase() + kDecisionCallRva);
        int32_t rel = 0;
        memcpy(&rel, call + 1, 4);
        const uintptr_t target = reinterpret_cast<uintptr_t>(call) + 5 + rel;
        if (call[0] != 0xE8u || target != ImageBase() + kDecisionRva)
        {
            m_failed = true;
            WOWVR_WARN("Interior view: 0x00783381 is not the expected call to 0x00795D40; "
                       "building interiors stay decided by the camera.");
            return false;
        }
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

        g_decision = reinterpret_cast<DecisionFn>(ImageBase() + kDecisionRva);
        if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &previous))
        {
            // The loads already read the eye WoWVR keeps; Update keeps it at the camera.
            m_failed = true;
            WOWVR_WARN("Interior view: 0x00783381 could not be made writable.");
            return false;
        }
        const int32_t toWrapper = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&DecisionWrapper) - (reinterpret_cast<uintptr_t>(call) + 5));
        memcpy(call + 1, &toWrapper, 4);
        VirtualProtect(call, 5, previous, &previous);
        FlushInstructionCache(GetCurrentProcess(), call, 5);
        m_installed = true;
        WOWVR_INFO("Interior view: the 'which building is the camera in' test (0x00795D40) "
                   "now reads its position from WoWVR.");
        return true;
    }

    void InteriorView::Update(bool fromCharacter, float cameraReachYards)
    {
        g_cameraReach = cameraReachYards;
        if (!m_installed)
        {
            if (m_failed)
            {
                // Half-installed (loads repointed, call not): keep the eye at the camera.
                memcpy(g_decisionEye, reinterpret_cast<const void*>(ImageBase() + kEyeRva),
                       sizeof(g_decisionEye));
            }
            return;
        }
        g_fromCharacter = fromCharacter;
        if (g_usedCharacter != m_lastFromCharacter)
        {
            m_lastFromCharacter = g_usedCharacter;
            WOWVR_INFO("Interior view: %s", g_usedCharacter
                ? "the character's surroundings are not reachable from the camera's; their "
                  "building groups are drawn as well."
                : "the camera's own view reaches the character again.");
        }
    }

    InteriorView& Interior()
    {
        static InteriorView instance;
        return instance;
    }
}
