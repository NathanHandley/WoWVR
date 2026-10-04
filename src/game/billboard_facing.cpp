#include "game/billboard_facing.h"

#include "core/log.h"

#include <windows.h>

#include <cstdint>
#include <cmath>
#include <cstring>

namespace wowvr
{
    namespace
    {
        const uintptr_t kPublishedImageBase = 0x00400000u;

        // CM2Scene animate. push ebp; mov ebp,esp; sub esp,0xF0
        const uintptr_t kSceneAnimateRva = 0x00821A20u - kPublishedImageBase;
        const uint8_t kSceneAnimateEntry[9] =
            { 0x55u, 0x8Bu, 0xECu, 0x81u, 0xECu, 0xF0u, 0x00u, 0x00u, 0x00u };

        // The two "jmp 0x008301FC" that end the spherical billboard case.
        const uintptr_t kJoinRva = 0x008301FCu - kPublishedImageBase;
        const uintptr_t kSphericalJumpRvas[2] = { 0x00830071u - kPublishedImageBase,
                                                  0x0083009Du - kPublishedImageBase };

        // Return addresses of the scene-animate calls whose scenes are drawn into the
        // world: world (0x0079A870), sky (0x007F08C0), the camera's two (0x006018C0,
        // 0x00601E90) and the login screen's (0x004D9660). Interface model frames -
        // CharacterModelBase, portraits, SetModel frames - are deliberately absent.
        const uintptr_t kWorldCallerReturnRvas[5] = {
            0x0079AC18u - kPublishedImageBase,
            0x007F0959u - kPublishedImageBase,
            0x00601929u - kPublishedImageBase,
            0x00601F08u - kPublishedImageBase,
            0x004D97BAu - kPublishedImageBase,
        };

        // The sun and moons are not M2s: 0x009AC660 draws each as a quad whose
        // orientation it builds (0x009ABB60) from the GAME camera's forward axis - the
        // third column of the view matrix - so the quad lies square to the camera's
        // axis, not to the line of sight. The call at 0x009AC884 is redirected so that
        // vector is replaced by the direction from the camera to the body first.
        // At the call: eax = &forward (3 floats), edi = the body (position at +0),
        // esi = the sky camera state (position at +0x18).
        const uintptr_t kCelestialBasisCallRva = 0x009AC884u - kPublishedImageBase;
        const uintptr_t kCelestialBasisRva = 0x009ABB60u - kPublishedImageBase;

        // World text - unit names over heads and floating combat text - is drawn by
        // 0x007E7490 with the view set to identity and, per item, a world matrix that is
        // a pure translation to the item's view-space position (identity, then
        // 0x004C1B30 translate-in-place at 0x007E7A66, ecx = the matrix). So each string
        // lies flat on the GAME camera's axes, tilting whenever the head and the camera
        // disagree. The call is redirected so that, after the translation, the matrix's
        // rotation becomes HC^T - the same turn the billboards get.
        const uintptr_t kWorldTextTranslateCallRva = 0x007E7A66u - kPublishedImageBase;
        const uintptr_t kTranslateInPlaceRva = 0x004C1B30u - kPublishedImageBase;

        // Read by the naked detours, which cannot reach a this-pointer.
        uintptr_t g_worldCallers[5] = {};
        volatile LONG g_sceneInWorld = 0;
        volatile LONG g_active = 0;
        uint8_t* g_sceneTrampoline = nullptr;
        uintptr_t g_joinAddress = 0;
        uintptr_t g_basisFunction = 0;
        uintptr_t g_translateInPlace = 0;
        unsigned long long g_celestialFacings = 0;
        bool g_installed = false;
        bool g_failed = false;

        // The head correction HC, applied as new row = row * HC^T.
        // Two copies swapped by index so a reader on another thread never sees half
        // of one frame's rotation and half of the next's.
        float g_rotation[2][3][3] = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } },
                                      { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
        volatile LONG g_rotationIndex = 0;

        uintptr_t ImageBase()
        {
            return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        }

        // Rows at +0x00, +0x10, +0x20 of a row-major 4x4, each a direction in the
        // game camera's view space. They are turned so that, after the head correction
        // the world receives, they come out as they were meant to look in the client's
        // own view: row * HC^T * HC = row.
        void __stdcall RotateBillboardRows(float* matrix)
        {
            const float (*m)[3] = g_rotation[g_rotationIndex & 1];
            for (int row = 0; row < 3; ++row)
            {
                float* r = matrix + row * 4;
                const float x = r[0];
                const float y = r[1];
                const float z = r[2];
                r[0] = x * m[0][0] + y * m[0][1] + z * m[0][2];
                r[1] = x * m[1][0] + y * m[1][1] + z * m[1][2];
                r[2] = x * m[2][0] + y * m[2][1] + z * m[2][2];
            }
        }

        typedef void(__stdcall* RotateRowsFn)(float*);
        RotateRowsFn g_rotateRows = RotateBillboardRows;

        // forward := normalize(body - camera). Left alone if the two coincide.
        void __stdcall FaceCelestialAtCamera(float* forward, const float* body,
                                             const uint8_t* skyCamera)
        {
            if (forward == nullptr || body == nullptr || skyCamera == nullptr)
            {
                return;
            }
            const float* camera = reinterpret_cast<const float*>(skyCamera + 0x18);
            const float x = body[0] - camera[0];
            const float y = body[1] - camera[1];
            const float z = body[2] - camera[2];
            const float lengthSquared = x * x + y * y + z * z;
            if (!(lengthSquared > 1.0e-6f))
            {
                return;
            }
            const float inverse = 1.0f / sqrtf(lengthSquared);
            forward[0] = x * inverse;
            forward[1] = y * inverse;
            forward[2] = z * inverse;
            ++g_celestialFacings;
        }

        typedef void(__stdcall* FaceCelestialFn)(float*, const float*, const uint8_t*);
        FaceCelestialFn g_faceCelestial = FaceCelestialAtCamera;

        // Called in place of 0x004C1B30 (thiscall: ecx = matrix, one stack argument, ret 4).
        // Runs the client's translate, then turns the matrix's rotation rows - which are
        // the identity here - into HC^T. The translation row is untouched.
        __declspec(naked) void WorldTextTranslateDetour()
        {
            __asm
            {
                push ecx                        // the matrix, kept across the call
                push dword ptr [esp + 8]        // the client's translation argument
                mov  eax, g_translateInPlace
                call eax                        // thiscall, pops its argument
                pop  ecx
                cmp  g_active, 0
                je   done
                pushad
                pushfd
                push ecx
                mov  eax, g_rotateRows
                call eax
                popfd
                popad
            done:
                ret  4
            }
        }

        // Called in place of 0x009ABB60; tail-jumps to it, so it returns straight to
        // the client's caller.
        __declspec(naked) void CelestialBasisDetour()
        {
            __asm
            {
                cmp  g_active, 0
                je   passthrough
                pushad
                pushfd
                push esi
                push edi
                push eax
                mov  eax, g_faceCelestial
                call eax
                popfd
                popad
            passthrough:
                jmp  dword ptr [g_basisFunction]
            }
        }

        __declspec(naked) void SceneAnimateDetour()
        {
            __asm
            {
                push eax
                mov  eax, [esp + 4]            // return address into the caller
                cmp  eax, g_worldCallers[0]
                je   inWorld
                cmp  eax, g_worldCallers[4]
                je   inWorld
                cmp  eax, g_worldCallers[8]
                je   inWorld
                cmp  eax, g_worldCallers[12]
                je   inWorld
                cmp  eax, g_worldCallers[16]
                je   inWorld
                mov  g_sceneInWorld, 0
                jmp  done
            inWorld:
                mov  g_sceneInWorld, 1
            done:
                pop  eax
                jmp  dword ptr [g_sceneTrampoline]
            }
        }

        // Entered in place of the spherical case's jump to the shared tail; edi is the
        // bone matrix whose rotation rows were just written.
        __declspec(naked) void SphericalBillboardDetour()
        {
            __asm
            {
                cmp  g_active, 0
                je   skip
                cmp  g_sceneInWorld, 0
                je   skip
                pushad
                pushfd
                push edi
                mov  eax, g_rotateRows
                call eax
                popfd
                popad
            skip:
                jmp  dword ptr [g_joinAddress]
            }
        }

        bool Patch(uint8_t* site, const uint8_t* bytes, size_t length)
        {
            DWORD previous = 0;
            if (!VirtualProtect(site, length, PAGE_EXECUTE_READWRITE, &previous))
            {
                return false;
            }
            memcpy(site, bytes, length);
            VirtualProtect(site, length, previous, &previous);
            FlushInstructionCache(GetCurrentProcess(), site, length);
            return true;
        }

        void JumpTo(uint8_t* out, uintptr_t from, const void* to)
        {
            out[0] = 0xE9u;
            const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - (from + 5));
            memcpy(out + 1, &rel, 4);
        }
    }

    bool BillboardFacing::Install()
    {
        if (g_installed)
        {
            return true;
        }
        if (g_failed)
        {
            return false;
        }

        const uintptr_t base = ImageBase();
        uint8_t* entry = reinterpret_cast<uint8_t*>(base + kSceneAnimateRva);
        const uintptr_t join = base + kJoinRva;

        // Everything is checked before anything is written: both jumps must still go
        // to the shared tail, and the entry must be the expected prologue.
        bool ok = memcmp(entry, kSceneAnimateEntry, sizeof(kSceneAnimateEntry)) == 0;
        uint8_t* celestialCall = reinterpret_cast<uint8_t*>(base + kCelestialBasisCallRva);
        uint8_t* worldTextCall = reinterpret_cast<uint8_t*>(base + kWorldTextTranslateCallRva);
        if (ok)
        {
            int32_t rel = 0;
            memcpy(&rel, worldTextCall + 1, 4);
            ok = worldTextCall[0] == 0xE8u
                && reinterpret_cast<uintptr_t>(worldTextCall) + 5 + static_cast<intptr_t>(rel)
                       == base + kTranslateInPlaceRva;
        }
        if (ok)
        {
            int32_t rel = 0;
            memcpy(&rel, celestialCall + 1, 4);
            ok = celestialCall[0] == 0xE8u
                && reinterpret_cast<uintptr_t>(celestialCall) + 5 + static_cast<intptr_t>(rel)
                       == base + kCelestialBasisRva;
        }
        for (int i = 0; i < 2 && ok; ++i)
        {
            const uint8_t* site = reinterpret_cast<const uint8_t*>(base + kSphericalJumpRvas[i]);
            int32_t rel = 0;
            memcpy(&rel, site + 1, 4);
            ok = site[0] == 0xE9u
                && reinterpret_cast<uintptr_t>(site) + 5 + static_cast<intptr_t>(rel) == join;
        }
        if (!ok)
        {
            g_failed = true;
            WOWVR_WARN("Billboard facing: the M2 billboard code is not what this was written "
                       "against; billboards keep facing the game camera.");
            return false;
        }

        for (int i = 0; i < 5; ++i)
        {
            g_worldCallers[i] = base + kWorldCallerReturnRvas[i];
        }
        g_joinAddress = join;
        g_basisFunction = base + kCelestialBasisRva;
        g_translateInPlace = base + kTranslateInPlaceRva;

        uint8_t* stub = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (stub == nullptr)
        {
            g_failed = true;
            WOWVR_WARN("Billboard facing: could not allocate the trampoline.");
            return false;
        }
        memcpy(stub, kSceneAnimateEntry, sizeof(kSceneAnimateEntry));
        JumpTo(stub + sizeof(kSceneAnimateEntry),
               reinterpret_cast<uintptr_t>(stub) + sizeof(kSceneAnimateEntry),
               entry + sizeof(kSceneAnimateEntry));
        FlushInstructionCache(GetCurrentProcess(), stub, sizeof(kSceneAnimateEntry) + 5);
        g_sceneTrampoline = stub;

        uint8_t patched[9];
        JumpTo(patched, reinterpret_cast<uintptr_t>(entry), &SceneAnimateDetour);
        for (size_t i = 5; i < sizeof(patched); ++i) { patched[i] = 0x90u; }
        if (!Patch(entry, patched, sizeof(patched)))
        {
            g_failed = true;
            WOWVR_WARN("Billboard facing: the scene animator could not be patched.");
            return false;
        }

        for (int i = 0; i < 2; ++i)
        {
            uint8_t* site = reinterpret_cast<uint8_t*>(base + kSphericalJumpRvas[i]);
            uint8_t jump[5];
            JumpTo(jump, reinterpret_cast<uintptr_t>(site), &SphericalBillboardDetour);
            if (!Patch(site, jump, sizeof(jump)))
            {
                // The entry hook alone changes nothing visible; leave it in place.
                g_failed = true;
                WOWVR_WARN("Billboard facing: a billboard jump could not be patched.");
                return false;
            }
        }

        {
            uint8_t call[5];
            JumpTo(call, reinterpret_cast<uintptr_t>(celestialCall), &CelestialBasisDetour);
            call[0] = 0xE8u;   // a call, not a jump: the detour returns through 0x009ABB60
            if (!Patch(celestialCall, call, sizeof(call)))
            {
                g_failed = true;
                WOWVR_WARN("Billboard facing: the sun and moon call could not be patched.");
                return false;
            }
        }

        {
            uint8_t call[5];
            JumpTo(call, reinterpret_cast<uintptr_t>(worldTextCall), &WorldTextTranslateDetour);
            call[0] = 0xE8u;
            if (!Patch(worldTextCall, call, sizeof(call)))
            {
                g_failed = true;
                WOWVR_WARN("Billboard facing: the world text call could not be patched.");
                return false;
            }
        }

        g_installed = true;
        WOWVR_INFO("Billboard facing: world text (unit names, combat text) now faces the "
                   "head; hook at 0x%08X.",
                   static_cast<unsigned>(reinterpret_cast<uintptr_t>(worldTextCall)));
        WOWVR_INFO("Billboard facing: the sun and moons, and spherical M2 billboards in the "
                   "world, now face the head. Sun/moon hook at 0x%08X.",
                   static_cast<unsigned>(reinterpret_cast<uintptr_t>(celestialCall)));
        WOWVR_INFO("Billboard facing: spherical M2 billboards in the world now face the "
                   "head (hooks at 0x%08X and the billboard tail).",
                   static_cast<unsigned>(reinterpret_cast<uintptr_t>(entry)));
        return true;
    }

    void BillboardFacing::Update(bool active, const Mat4& gameViewToHead)
    {
        // new row = row * HC^T, i.e. new[j] = sum_k row[k] * HC[j][k]: store HC as is
        // and index it [j][k] in RotateBillboardRows.
        const LONG next = (g_rotationIndex + 1) & 1;
        for (int j = 0; j < 3; ++j)
        {
            for (int k = 0; k < 3; ++k)
            {
                g_rotation[next][j][k] = gameViewToHead.m[j][k];
            }
        }
        InterlockedExchange(&g_rotationIndex, next);
        InterlockedExchange(&g_active, (active && g_installed) ? 1 : 0);
    }

    void BillboardFacing::LogStatus() const
    {
        WOWVR_INFO("Billboard facing: %s; %llu sun/moon quads turned to the viewer since the "
                   "last report.", g_installed ? (g_active ? "active" : "installed, idle")
                                               : (g_failed ? "not installed" : "pending"),
                   g_celestialFacings);
        g_celestialFacings = 0;
    }

    BillboardFacing& Billboards()
    {
        static BillboardFacing instance;
        return instance;
    }
}
